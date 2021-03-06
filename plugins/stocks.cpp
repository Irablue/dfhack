#include "uicommon.h"

#include <functional>

// DF data structure definition headers
#include "DataDefs.h"
#include "Types.h"

#include "df/item.h"
#include "df/viewscreen_dwarfmodest.h"
#include "df/items_other_id.h"
#include "df/job.h"
#include "df/unit.h"
#include "df/world.h"
#include "df/item_quality.h"
#include "df/caravan_state.h"
#include "df/mandate.h"
#include "df/general_ref_building_holderst.h"

#include "modules/Gui.h"
#include "modules/Items.h"
#include "modules/Job.h"
#include "modules/World.h"
#include "modules/Screen.h"
#include "modules/Maps.h"

using df::global::world;

DFHACK_PLUGIN("stocks");
#define PLUGIN_VERSION 0.2

DFhackCExport command_result plugin_shutdown ( color_ostream &out )
{
    return CR_OK;
}

#define MAX_NAME 30
#define SIDEBAR_WIDTH 30

static bool show_debugging = false;

static void debug(const string &msg)
{
    if (!show_debugging)
        return;

    color_ostream_proxy out(Core::getInstance().getConsole());
    out << "DEBUG (stocks): " << msg << endl;
}


/*
 * Utility
 */

static string get_quality_name(const df::item_quality quality)
{
    if (gps->dimx - SIDEBAR_WIDTH < 60)
        return int_to_string(quality);
    else
        return ENUM_KEY_STR(item_quality, quality);
}


/*
 * Trade
 */

static df::job *get_item_job(df::item *item)
{
    auto ref = Items::getSpecificRef(item, specific_ref_type::JOB);
    if (ref && ref->job)
        return ref->job;

    return nullptr;
}

static df::item *get_container_of(df::item *item)
{
    auto container = Items::getContainer(item);
    return (container) ? container : item;
}

static bool is_marked_for_trade(df::item *item, df::item *container = nullptr)
{
    item = (container) ? container : get_container_of(item);
    auto job = get_item_job(item);
    if (!job)
        return false;

    return job->job_type == job_type::BringItemToDepot;
}

static bool check_mandates(df::item *item)
{
    for (auto it = world->mandates.begin(); it != world->mandates.end(); it++)
    {
        auto mandate = *it;

        if (mandate->mode != 0)
            continue;

        if (item->getType() != mandate->item_type || 
            (mandate->item_subtype != -1 && item->getSubtype() != mandate->item_subtype))
            continue;

        if (mandate->mat_type != -1 && item->getMaterial() != mandate->mat_type)
            continue;

        if (mandate->mat_index != -1 && item->getMaterialIndex() != mandate->mat_index)
            continue;

        return false;
    }

    return true;
}

static bool can_trade_item(df::item *item)
{
    if (item->flags.bits.owned || item->flags.bits.artifact || item->flags.bits.spider_web || item->flags.bits.in_job)
        return false;

    for (size_t i = 0; i < item->general_refs.size(); i++)
    {
        df::general_ref *ref = item->general_refs[i];

        switch (ref->getType())
        {
        case general_ref_type::UNIT_HOLDER:
            return false;

        case general_ref_type::BUILDING_HOLDER:
            return false;

        default:
            break;
        }
    }

    for (size_t i = 0; i < item->specific_refs.size(); i++)
    {
        df::specific_ref *ref = item->specific_refs[i];

        if (ref->type == specific_ref_type::JOB)
        {
            // Ignore any items assigned to a job
            return false;
        }
    }

    return check_mandates(item);
}

static bool can_trade_item_and_container(df::item *item)
{
    item = get_container_of(item);

    if (item->flags.bits.in_inventory)
        return false;

    if (!can_trade_item(item))
        return false;

    vector<df::item*> contained_items;
    Items::getContainedItems(item, &contained_items);
    for (auto cit = contained_items.begin(); cit != contained_items.end(); cit++)
    {
        if (!can_trade_item(*cit))
            return false;
    }

    return true;
}

static bool is_in_inventory(df::item *item)
{
    item = get_container_of(item);
    return item->flags.bits.in_inventory;
}


class TradeDepotInfo
{
public:
    TradeDepotInfo()
    {
        reset();
    }

    void prepareTradeVarables()
    {
        reset();
        for(auto bld_it = world->buildings.all.begin(); bld_it != world->buildings.all.end(); bld_it++)
        {
            auto bld = *bld_it;
            if (!isUsableDepot(bld))
                continue;

            depot = bld;
            id = depot->id;
            trade_possible = caravansAvailable();
            break;
        }
    }

    bool assignItem(df::item *item)
    {
        item = get_container_of(item);
        if (!can_trade_item_and_container(item))
            return false;

        auto href = df::allocate<df::general_ref_building_holderst>();
        if (!href)
            return false;

        auto job = new df::job();

        df::coord tpos(depot->centerx, depot->centery, depot->z);
        job->pos = tpos;

        job->job_type = job_type::BringItemToDepot;

        // job <-> item link
        if (!Job::attachJobItem(job, item, df::job_item_ref::Hauled))
        {
            delete job;
            delete href;
            return false;
        }

        // job <-> building link
        href->building_id = id;
        depot->jobs.push_back(job);
        job->general_refs.push_back(href);

        // add to job list
        Job::linkIntoWorld(job);

        return true;
    }

    void reset()
    {
        depot = 0;
        trade_possible = false;
    }

    bool canTrade()
    {
        return trade_possible;
    }

private:
    int32_t id;
    df::building *depot;
    bool trade_possible;

    bool isUsableDepot(df::building* bld)
    {
        if (bld->getType() != building_type::TradeDepot)
            return false;

        if (bld->getBuildStage() < bld->getMaxBuildStage())
            return false;

        if (bld->jobs.size() == 1 && bld->jobs[0]->job_type == job_type::DestroyBuilding)
            return false;

        return true;
    }

    bool caravansAvailable()
    {
        if (df::global::ui->caravans.size() == 0)
            return false;

        for (auto it = df::global::ui->caravans.begin(); it != df::global::ui->caravans.end(); it++)
        {
            auto caravan = *it;
            auto trade_state = caravan->trade_state;
            auto time_remaining = caravan->time_remaining;
            if ((trade_state != 1 && trade_state != 2) || time_remaining == 0)
                return false;
        }

        return true;
    }
};

static TradeDepotInfo depot_info;


static string get_keywords(df::item *item)
{
    string keywords;

    if (item->flags.bits.in_job)
        keywords += "job ";

    if (item->flags.bits.rotten)
        keywords += "rotten ";

    if (item->flags.bits.foreign)
        keywords += "foreign ";

    if (item->flags.bits.owned)
        keywords += "owned ";

    if (item->flags.bits.forbid)
        keywords += "forbid ";

    if (item->flags.bits.dump)
        keywords += "dump ";

    if (item->flags.bits.on_fire)
        keywords += "fire ";

    if (item->flags.bits.melt)
        keywords += "melt ";

    if (is_in_inventory(item))
        keywords += "inventory ";

    if (depot_info.canTrade())
    {
        if (is_marked_for_trade(item))
            keywords += "trade ";
    }

    return keywords;
}

template <class T>
class StockListColumn : public ListColumn<T>
{
    virtual void display_extras(const T &item, int32_t &x, int32_t &y) const
    {
        if (item->flags.bits.in_job)
            OutputString(COLOR_LIGHTBLUE, x, y, "J");
        else
            OutputString(COLOR_LIGHTBLUE, x, y, " ");

        if (item->flags.bits.rotten)
            OutputString(COLOR_CYAN, x, y, "X");
        else
            OutputString(COLOR_LIGHTBLUE, x, y, " ");

        if (item->flags.bits.foreign)
            OutputString(COLOR_BROWN, x, y, "G");
        else
            OutputString(COLOR_LIGHTBLUE, x, y, " ");

        if (item->flags.bits.owned)
            OutputString(COLOR_GREEN, x, y, "O");
        else
            OutputString(COLOR_LIGHTBLUE, x, y, " ");

        if (item->flags.bits.forbid)
            OutputString(COLOR_RED, x, y, "F");
        else
            OutputString(COLOR_LIGHTBLUE, x, y, " ");

        if (item->flags.bits.dump)
            OutputString(COLOR_LIGHTMAGENTA, x, y, "D");
        else
            OutputString(COLOR_LIGHTBLUE, x, y, " ");
        
        if (item->flags.bits.on_fire)
            OutputString(COLOR_LIGHTRED, x, y, "R");
        else
            OutputString(COLOR_LIGHTBLUE, x, y, " ");
        
        if (item->flags.bits.melt)
            OutputString(COLOR_BLUE, x, y, "M");
        else
            OutputString(COLOR_LIGHTBLUE, x, y, " ");

        if (is_in_inventory(item))
            OutputString(COLOR_WHITE, x, y, "I");
        else
            OutputString(COLOR_LIGHTBLUE, x, y, " ");

        if (depot_info.canTrade())
        {
            if (is_marked_for_trade(item))
                OutputString(COLOR_LIGHTGREEN, x, y, "T");
            else
                OutputString(COLOR_LIGHTBLUE, x, y, " ");
        }

        if (item->isImproved())
            OutputString(COLOR_BLUE, x, y, "* ");
        else
            OutputString(COLOR_LIGHTBLUE, x, y, "  ");

        auto quality = static_cast<df::item_quality>(item->getQuality());
        if (quality > item_quality::Ordinary)
        {
            auto color = COLOR_BROWN;
            switch(quality)
            {
            case item_quality::FinelyCrafted:
                color = COLOR_CYAN;
                break;

            case item_quality::Superior:
                color = COLOR_LIGHTBLUE;
                break;

            case item_quality::Exceptional:
                color = COLOR_GREEN;
                break;

            case item_quality::Masterful:
                color = COLOR_LIGHTGREEN;
                break;

            case item_quality::Artifact:
                color = COLOR_BLUE;
                break;

            default:
                break;
            }
            OutputString(color, x, y, get_quality_name(quality));
        }
    }
};

struct extra_filters
{
    bool hide_trade_marked, hide_in_inventory;

    extra_filters()
    {
        reset();
    }

    void reset()
    {
        hide_in_inventory = false;
        hide_trade_marked = false;
    }
};

class ViewscreenStocks : public dfhack_viewscreen
{
public:
    static df::item_flags hide_flags;
    static extra_filters extra_hide_flags;

    ViewscreenStocks()
    {
        selected_column = 0;
        items_column.setTitle("Item");
        items_column.multiselect = false;
        items_column.auto_select = true;
        items_column.allow_search = true;
        items_column.left_margin = 2;
        items_column.bottom_margin = 1;
        items_column.search_margin = gps->dimx - SIDEBAR_WIDTH;

        items_column.changeHighlight(0);

        apply_to_all = false;
        hide_unflagged = false;
        
        checked_flags.bits.in_job = true;
        checked_flags.bits.rotten = true;
        checked_flags.bits.foreign = true;
        checked_flags.bits.owned = true;
        checked_flags.bits.forbid = true;
        checked_flags.bits.dump = true;
        checked_flags.bits.on_fire = true;
        checked_flags.bits.melt = true;
        checked_flags.bits.on_fire = true;

        min_quality = item_quality::Ordinary;
        max_quality = item_quality::Artifact;
        min_wear = 0;

        populateItems();

        items_column.selectDefaultEntry();
    }

    static void reset()
    {
        hide_flags.whole = 0;
        extra_hide_flags.reset();
        depot_info.reset();
    }

    void feed(set<df::interface_key> *input)
    {
        bool key_processed = false;
        switch (selected_column)
        {
        case 0:
            key_processed = items_column.feed(input);
            break;
        }

        if (key_processed)
            return;

        if (input->count(interface_key::LEAVESCREEN))
        {
            input->clear();
            Screen::dismiss(this);
            return;
        }

        if (input->count(interface_key::CUSTOM_CTRL_G))
        {
            hide_flags.bits.foreign = !hide_flags.bits.foreign;
            populateItems();
        }
        else if (input->count(interface_key::CUSTOM_CTRL_J))
        {
            hide_flags.bits.in_job = !hide_flags.bits.in_job;
            populateItems();
        }
        else if (input->count(interface_key::CUSTOM_CTRL_X))
        {
            hide_flags.bits.rotten = !hide_flags.bits.rotten;
            populateItems();
        }
        else if (input->count(interface_key::CUSTOM_CTRL_O))
        {
            hide_flags.bits.owned = !hide_flags.bits.owned;
            populateItems();
        }
        else if (input->count(interface_key::CUSTOM_CTRL_F))
        {
            hide_flags.bits.forbid = !hide_flags.bits.forbid;
            populateItems();
        }
        else if (input->count(interface_key::CUSTOM_CTRL_D))
        {
            hide_flags.bits.dump = !hide_flags.bits.dump;
            populateItems();
        }
        else if (input->count(interface_key::CUSTOM_CTRL_R))
        {
            hide_flags.bits.on_fire = !hide_flags.bits.on_fire;
            populateItems();
        }
        else if (input->count(interface_key::CUSTOM_CTRL_M))
        {
            hide_flags.bits.melt = !hide_flags.bits.melt;
            populateItems();
        }
        else if (input->count(interface_key::CUSTOM_CTRL_I))
        {
            extra_hide_flags.hide_in_inventory = !extra_hide_flags.hide_in_inventory;
            populateItems();
        }
        else if (input->count(interface_key::CUSTOM_CTRL_T))
        {
            extra_hide_flags.hide_trade_marked = !extra_hide_flags.hide_trade_marked;
            populateItems();
        }
        else if (input->count(interface_key::CUSTOM_CTRL_N))
        {
            hide_unflagged = !hide_unflagged;
            populateItems();
        }
        else if (input->count(interface_key::CUSTOM_SHIFT_C))
        {
            setAllFlags(true);
            populateItems();
        }
        else if (input->count(interface_key::CUSTOM_SHIFT_E))
        {
            setAllFlags(false);
            populateItems();
        }
        else if (input->count(interface_key::SECONDSCROLL_UP))
        {
            if (min_quality > item_quality::Ordinary)
            {
                min_quality = static_cast<df::item_quality>(static_cast<int16_t>(min_quality) - 1);
                populateItems();
            }
        }
        else if (input->count(interface_key::SECONDSCROLL_DOWN))
        {
            if (min_quality < max_quality && min_quality < item_quality::Artifact)
            {
                min_quality = static_cast<df::item_quality>(static_cast<int16_t>(min_quality) + 1);
                populateItems();
            }
        }
        else if (input->count(interface_key::SECONDSCROLL_PAGEUP))
        {
            if (max_quality > min_quality && max_quality > item_quality::Ordinary)
            {
                max_quality = static_cast<df::item_quality>(static_cast<int16_t>(max_quality) - 1);
                populateItems();
            }
        }
        else if (input->count(interface_key::SECONDSCROLL_PAGEDOWN))
        {
            if (max_quality < item_quality::Artifact)
            {
                max_quality = static_cast<df::item_quality>(static_cast<int16_t>(max_quality) + 1);
                populateItems();
            }
        }
        else if (input->count(interface_key::CUSTOM_SHIFT_W))
        {
            ++min_wear;
            if (min_wear > 3)
                min_wear = 0;

            populateItems();
        }

        else if (input->count(interface_key::CUSTOM_SHIFT_Z))
        {
            input->clear();
            auto item = items_column.getFirstSelectedElem();
            if (!item)
                return;
            auto pos = getRealPos(item);
            if (!pos)
                return;

            Screen::dismiss(this);
            // Could be clever here, if item is in a container, to look inside the container.
            // But that's different for built containers vs bags/pots in stockpiles.
            send_key(interface_key::D_LOOK);
            move_cursor(*pos);
        }
        else if (input->count(interface_key::CUSTOM_SHIFT_A))
        {
            apply_to_all = !apply_to_all;
        }
        else if (input->count(interface_key::CUSTOM_SHIFT_D))
        {
            df::item_flags flags;
            flags.bits.dump = true;
            applyFlag(flags);
            populateItems();
        }
        else if (input->count(interface_key::CUSTOM_SHIFT_F))
        {
            df::item_flags flags;
            flags.bits.forbid = true;
            applyFlag(flags);
            populateItems();
        }
        else if (input->count(interface_key::CUSTOM_SHIFT_T))
        {
            if (apply_to_all)
            {
                auto &list = items_column.getDisplayList();
                for (auto iter = list.begin(); iter != list.end(); iter++)
                {
                    auto item = (*iter)->elem;
                    if (item)
                        depot_info.assignItem(item);
                }

                populateItems();
            }
            else
            {
                auto item = items_column.getFirstSelectedElem();
                if (item && depot_info.assignItem(item))
                    populateItems();
            }
        }

        else if (input->count(interface_key::CURSOR_LEFT))
        {
            --selected_column;
            validateColumn();
        }
        else if (input->count(interface_key::CURSOR_RIGHT))
        {
            selected_column++;
            validateColumn();
        }
        else if (enabler->tracking_on && enabler->mouse_lbut)
        {
            if (items_column.setHighlightByMouse())
                selected_column = 0;

            enabler->mouse_lbut = enabler->mouse_rbut = 0;
        }
    }

    void move_cursor(const df::coord &pos)
    {
        Gui::setCursorCoords(pos.x, pos.y, pos.z);
        send_key(interface_key::CURSOR_DOWN_Z);
        send_key(interface_key::CURSOR_UP_Z);
    }

    void send_key(const df::interface_key &key)
    {
        set< df::interface_key > keys;
        keys.insert(key);
        Gui::getCurViewscreen(true)->feed(&keys);
    }

    void render()
    {
        if (Screen::isDismissed(this))
            return;

        dfhack_viewscreen::render();

        Screen::clear();
        Screen::drawBorder("  Stocks  ");

        items_column.display(selected_column == 0);

        int32_t y = 1;
        auto left_margin = gps->dimx - SIDEBAR_WIDTH;
        int32_t x = left_margin - 2;
        Screen::Pen border('\xDB', 8);
        for (; y < gps->dimy - 1; y++)
        {
            paintTile(border, x, y);
        }

        y = 2;
        x = left_margin;
        OutputString(COLOR_BROWN, x, y, "Filters", true, left_margin);
        OutputString(COLOR_LIGHTRED, x, y, "Press Ctrl-Hotkey to Toggle", true, left_margin);
        OutputFilterString(x, y, "In Job", "J", !hide_flags.bits.in_job, true, left_margin, COLOR_LIGHTBLUE);
        OutputFilterString(x, y, "Rotten", "X", !hide_flags.bits.rotten, true, left_margin, COLOR_CYAN);
        OutputFilterString(x, y, "Foreign Made", "G", !hide_flags.bits.foreign, true, left_margin, COLOR_BROWN);
        OutputFilterString(x, y, "Owned", "O", !hide_flags.bits.owned, true, left_margin, COLOR_GREEN);
        OutputFilterString(x, y, "Forbidden", "F", !hide_flags.bits.forbid, true, left_margin, COLOR_RED);
        OutputFilterString(x, y, "Dump", "D", !hide_flags.bits.dump, true, left_margin, COLOR_LIGHTMAGENTA);
        OutputFilterString(x, y, "On Fire", "R", !hide_flags.bits.on_fire, true, left_margin, COLOR_LIGHTRED);
        OutputFilterString(x, y, "Melt", "M", !hide_flags.bits.melt, true, left_margin, COLOR_BLUE);
        OutputFilterString(x, y, "In Inventory", "I", !extra_hide_flags.hide_in_inventory, true, left_margin, COLOR_WHITE);
        OutputFilterString(x, y, "Trade", "T", !extra_hide_flags.hide_trade_marked, true, left_margin, COLOR_LIGHTGREEN);
        OutputFilterString(x, y, "No Flags", "N", !hide_unflagged, true, left_margin, COLOR_GREY);
        ++y;
        OutputHotkeyString(x, y, "Clear All", "Shift-C", true, left_margin);
        OutputHotkeyString(x, y, "Enable All", "Shift-E", true, left_margin);
        ++y;
        OutputHotkeyString(x, y, "Min Qual: ", "-+");
        OutputString(COLOR_BROWN, x, y, get_quality_name(min_quality), true, left_margin);
        OutputHotkeyString(x, y, "Max Qual: ", "/*");
        OutputString(COLOR_BROWN, x, y, get_quality_name(max_quality), true, left_margin);

        ++y;
        OutputHotkeyString(x, y, "Min Wear: ", "Shift-W");
        OutputString(COLOR_BROWN, x, y, int_to_string(min_wear), true, left_margin);
        
        ++y;
        OutputString(COLOR_BROWN, x, y, "Actions (");
        OutputString(COLOR_LIGHTGREEN, x, y, int_to_string(items_column.getDisplayedListSize()));
        OutputString(COLOR_BROWN, x, y, " Items)", true, left_margin);
        OutputHotkeyString(x, y, "Zoom", "Shift-Z", true, left_margin);
        OutputHotkeyString(x, y, "Apply to: ", "Shift-A");
        OutputString(COLOR_BROWN, x, y, (apply_to_all) ? "Listed" : "Selected", true, left_margin);
        OutputHotkeyString(x, y, "Dump", "Shift-D", true, left_margin);
        OutputHotkeyString(x, y, "Forbid", "Shift-F", true, left_margin);
        OutputHotkeyString(x, y, "Mark for Trade", "Shift-T", true, left_margin);
    }

    std::string getFocusString() { return "stocks_view"; }

private:
    StockListColumn<df::item *> items_column;
    int selected_column;
    bool apply_to_all, hide_unflagged;
    df::item_flags checked_flags;
    df::item_quality min_quality, max_quality;
    int16_t min_wear;

    static df::coord *getRealPos(df::item *item)
    {
        item = get_container_of(item);
        if (item->flags.bits.in_inventory)
        {
            if (item->flags.bits.in_job)
            {
                auto ref = Items::getSpecificRef(item, specific_ref_type::JOB);
                if (ref && ref->job)
                {
                    if (ref->job->job_type == job_type::Eat || ref->job->job_type == job_type::Drink)
                        return nullptr;

                    auto unit = Job::getWorker(ref->job);
                    if (unit)
                        return &unit->pos;
                }
                return nullptr;
            }
            else
            {
                auto unit = Items::getHolderUnit(item);
                if (unit)
                    return &unit->pos;

                return nullptr;
            }
        }

        return &item->pos;
    }

    void applyFlag(const df::item_flags flags)
    {
        if (apply_to_all)
        {
            int state_to_apply = -1;
            for (auto iter = items_column.getDisplayList().begin(); iter != items_column.getDisplayList().end(); iter++)
            {
                auto item = (*iter)->elem;
                if (item)
                {
                    // Set all flags based on state of first item in list
                    if (state_to_apply == -1)
                        state_to_apply = (item->flags.whole & flags.whole) ? 0 : 1;

                    if (state_to_apply)
                        item->flags.whole |= flags.whole;
                    else
                        item->flags.whole &= ~flags.whole;
                }
            }
        }
        else
        {
            auto item = items_column.getFirstSelectedElem();
            if (item)
                item->flags.whole ^= flags.whole;
        }
    }

    void setAllFlags(bool state)
    {
        hide_flags.bits.in_job = state;
        hide_flags.bits.rotten = state;
        hide_flags.bits.foreign = state;
        hide_flags.bits.owned = state;
        hide_flags.bits.forbid = state;
        hide_flags.bits.dump = state;
        hide_flags.bits.on_fire = state;
        hide_flags.bits.melt = state;
        hide_flags.bits.on_fire = state;
        hide_unflagged = state;
        extra_hide_flags.hide_trade_marked = state;
        extra_hide_flags.hide_in_inventory = state;
    }

    void populateItems()
    {
        items_column.clear();

        df::item_flags bad_flags;
        bad_flags.whole = 0;
        bad_flags.bits.hostile = true;
        bad_flags.bits.trader = true;
        bad_flags.bits.in_building = true;
        bad_flags.bits.garbage_collect = true;
        bad_flags.bits.hostile = true;
        bad_flags.bits.removed = true;
        bad_flags.bits.dead_dwarf = true;
        bad_flags.bits.murder = true;
        bad_flags.bits.construction = true;

        depot_info.prepareTradeVarables();

        std::vector<df::item*> &items = world->items.other[items_other_id::IN_PLAY];

        for (size_t i = 0; i < items.size(); i++)
        {
            df::item *item = items[i];

            if (item->flags.whole & bad_flags.whole || item->flags.whole & hide_flags.whole)
                continue;

            auto container = get_container_of(item);
            if (container->flags.whole & bad_flags.whole)
                continue;

            auto pos = getRealPos(item);
            if (!pos)
                continue;

            if (pos->x == -30000)
                continue;

            auto designation = Maps::getTileDesignation(*pos);
            if (!designation)
                continue;

            if (designation->bits.hidden)
                continue; // Items in parts of the map not yet revealed

            bool trade_marked = is_marked_for_trade(item, container);
            if (extra_hide_flags.hide_trade_marked && trade_marked)
                continue;

            if (extra_hide_flags.hide_in_inventory && container->flags.bits.in_inventory)
                continue;

            if (hide_unflagged && (!(item->flags.whole & checked_flags.whole) && 
                    !trade_marked && !container->flags.bits.in_inventory))
            {
                continue;
            }

            auto quality = static_cast<df::item_quality>(item->getQuality());
            if (quality < min_quality || quality > max_quality)
                continue;

            auto wear = item->getWear();
            if (wear < min_wear)
                continue;

            auto label = Items::getDescription(item, 0, false);
            if (wear > 0)
            {
                string wearX;
                switch (wear)
                {
                case 1:
                    wearX = "x";
                    break;

                case 2:
                    wearX = "X";
                    break;

                case 3:
                    wearX = "xX";
                    break;

                default:
                    wearX = "XX";
                    break;

                }

                label = wearX + label + wearX;
            }

            label = pad_string(label, MAX_NAME, false, true);

            auto entry = ListEntry<df::item *>(label, item, get_keywords(item));
            items_column.add(entry);
        }

        items_column.filterDisplay();
    }

    void validateColumn()
    {
        set_to_limit(selected_column, 0);
    }

    void resize(int32_t x, int32_t y)
    {
        dfhack_viewscreen::resize(x, y);
        items_column.resize();
        items_column.search_margin = gps->dimx - SIDEBAR_WIDTH;
    }
};

df::item_flags ViewscreenStocks::hide_flags;
extra_filters ViewscreenStocks::extra_hide_flags;


static command_result stocks_cmd(color_ostream &out, vector <string> & parameters)
{
    if (!parameters.empty())
    {
        if (toLower(parameters[0])[0] == 'v')
        {
            out << "Stocks plugin" << endl << "Version: " << PLUGIN_VERSION << endl;
            return CR_OK;
        }
        else if (toLower(parameters[0])[0] == 's')
        {
            Screen::show(new ViewscreenStocks());
            return CR_OK;
        }
    }

    return CR_WRONG_USAGE;
}


DFhackCExport command_result plugin_init ( color_ostream &out, std::vector <PluginCommand> &commands)
{
    if (!gps)
        out.printerr("Could not insert stocks plugin hooks!\n");

    commands.push_back(
        PluginCommand(
        "stocks", "An improved stocks display screen",
        stocks_cmd, false, ""));

    ViewscreenStocks::reset();

    return CR_OK;
}


DFhackCExport command_result plugin_onstatechange(color_ostream &out, state_change_event event)
{
    switch (event) {
    case SC_MAP_LOADED:
        ViewscreenStocks::reset();
        break;
    default:
        break;
    }

    return CR_OK;
}
