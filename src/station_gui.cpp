/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file station_gui.cpp The GUI for stations. */

#include "stdafx.h"
#include "debug.h"
#include "gui.h"
#include "textbuf_gui.h"
#include "company_func.h"
#include "command_func.h"
#include "vehicle_gui.h"
#include "cargotype.h"
#include "station_gui.h"
#include "strings_func.h"
#include "string_func.h"
#include "window_func.h"
#include "viewport_func.h"
#include "dropdown_type.h"
#include "dropdown_func.h"
#include "station_base.h"
#include "waypoint_base.h"
#include "tilehighlight_func.h"
#include "company_base.h"
#include "sortlist_type.h"
#include "core/geometry_func.hpp"
#include "vehiclelist.h"
#include "town.h"
#include "linkgraph/linkgraph.h"
#include "zoom_func.h"
#include "station_cmd.h"

#include "widgets/station_widget.h"

#include "table/strings.h"

#include "dropdown_common_type.h"

#include "safeguards.h"

struct StationTypeFilter
{
	using StationType = Station;

	static bool IsValidID(StationID id) { return Station::IsValidID(id); }
	static bool IsValidBaseStation(const BaseStation *st) { return Station::IsExpected(st); }
	static bool IsAcceptableWaypointTile(TileIndex) { return false; }
	static constexpr bool IsWaypoint() { return false; }
};

template <bool ROAD, TileType TILE_TYPE>
struct GenericWaypointTypeFilter
{
	using StationType = Waypoint;

	static bool IsValidID(StationID id) { return Waypoint::IsValidID(id) && HasBit(Waypoint::Get(id)->waypoint_flags, WPF_ROAD) == ROAD; }
	static bool IsValidBaseStation(const BaseStation *st) { return Waypoint::IsExpected(st) && HasBit(Waypoint::From(st)->waypoint_flags, WPF_ROAD) == ROAD; }
	static bool IsAcceptableWaypointTile(TileIndex tile) { return IsTileType(tile, TILE_TYPE); }
	static constexpr bool IsWaypoint() { return true; }
};
using RailWaypointTypeFilter = GenericWaypointTypeFilter<false, MP_RAILWAY>;
using RoadWaypointTypeFilter = GenericWaypointTypeFilter<true, MP_ROAD>;

/**
 * Calculates and draws the accepted or supplied cargo around the selected tile(s)
 * @param r Rect where the string is to be drawn.
 * @param sct which type of cargo is to be displayed (passengers/non-passengers)
 * @param rad radius around selected tile(s) to be searched
 * @param supplies if supplied cargoes should be drawn, else accepted cargoes
 * @return Returns the y value below the string that was drawn
 */
int DrawStationCoverageAreaText(const Rect &r, StationCoverageType sct, int rad, bool supplies)
{
	TileIndex tile = TileVirtXY(_thd.pos.x, _thd.pos.y);
	CargoTypes cargo_mask = 0;
	if (_thd.drawstyle == HT_RECT && tile < Map::Size()) {
		CargoArray cargoes;
		if (supplies) {
			cargoes = GetProductionAroundTiles(tile, _thd.size.x / TILE_SIZE, _thd.size.y / TILE_SIZE, rad);
		} else {
			cargoes = GetAcceptanceAroundTiles(tile, _thd.size.x / TILE_SIZE, _thd.size.y / TILE_SIZE, rad);
		}

		/* Convert cargo counts to a set of cargo bits, and draw the result. */
		for (CargoType cargo = 0; cargo < NUM_CARGO; ++cargo) {
			switch (sct) {
				case SCT_PASSENGERS_ONLY: if (!IsCargoInClass(cargo, CargoClass::Passengers)) continue; break;
				case SCT_NON_PASSENGERS_ONLY: if (IsCargoInClass(cargo, CargoClass::Passengers)) continue; break;
				case SCT_ALL: break;
				default: NOT_REACHED();
			}
			if (cargoes[cargo] >= (supplies ? 1U : 8U)) SetBit(cargo_mask, cargo);
		}
	}
	return DrawStringMultiLine(r, GetString(supplies ? STR_STATION_BUILD_SUPPLIES_CARGO : STR_STATION_BUILD_ACCEPTS_CARGO, cargo_mask));
}

/**
 * Find stations adjacent to the current tile highlight area, so that existing coverage
 * area can be drawn.
 */
template <typename T>
void FindStationsAroundSelection()
{
	/* With distant join we don't know which station will be selected, so don't show any */
	if (_ctrl_pressed) {
		SetViewportCatchmentSpecializedStation<typename T::StationType>(nullptr, true);
		return;
	}

	/* Tile area for TileHighlightData */
	TileArea location(TileVirtXY(_thd.pos.x, _thd.pos.y), _thd.size.x / TILE_SIZE - 1, _thd.size.y / TILE_SIZE - 1);

	/* If the current tile is already a station, then it must be the nearest station. */
	if (IsTileType(location.tile, MP_STATION) && GetTileOwner(location.tile) == _local_company) {
		typename T::StationType *st = T::StationType::GetByTile(location.tile);
		if (st != nullptr && T::IsValidBaseStation(st)) {
			SetViewportCatchmentSpecializedStation<typename T::StationType>(st, true);
			return;
		}
	}

	/* Extended area by one tile */
	uint x = TileX(location.tile);
	uint y = TileY(location.tile);

	/* Waypoints can only be built on existing rail/road tiles, so don't extend area if not highlighting a rail tile. */
	int max_c = T::IsWaypoint() && !T::IsAcceptableWaypointTile(location.tile) ? 0 : 1;
	TileArea ta(TileXY(std::max<int>(0, x - max_c), std::max<int>(0, y - max_c)), TileXY(std::min<int>(Map::MaxX(), x + location.w + max_c), std::min<int>(Map::MaxY(), y + location.h + max_c)));

	typename T::StationType *adjacent = nullptr;

	/* Direct loop instead of ForAllStationsAroundTiles as we are not interested in catchment area */
	for (TileIndex tile : ta) {
		if (IsTileType(tile, MP_STATION) && GetTileOwner(tile) == _local_company) {
			typename T::StationType *st = T::StationType::GetByTile(tile);
			if (st == nullptr || !T::IsValidBaseStation(st)) continue;
			if (adjacent != nullptr && st != adjacent) {
				/* Multiple nearby, distant join is required. */
				adjacent = nullptr;
				break;
			}
			adjacent = st;
		}
	}
	SetViewportCatchmentSpecializedStation<typename T::StationType>(adjacent, true);
}

/**
 * Check whether we need to redraw the station coverage text.
 * If it is needed actually make the window for redrawing.
 * @param w the window to check.
 */
void CheckRedrawStationCoverage(const Window *w)
{
	/* Test if ctrl state changed */
	static bool _last_ctrl_pressed;
	if (_ctrl_pressed != _last_ctrl_pressed) {
		_thd.dirty = 0xff;
		_last_ctrl_pressed = _ctrl_pressed;
	}

	if (_thd.dirty & 1) {
		_thd.dirty &= ~1;
		w->SetDirty();

		if (_settings_client.gui.station_show_coverage && _thd.drawstyle == HT_RECT) {
			FindStationsAroundSelection<StationTypeFilter>();
		}
	}
}

template <typename T>
void CheckRedrawWaypointCoverage()
{
	/* Test if ctrl state changed */
	static bool _last_ctrl_pressed;
	if (_ctrl_pressed != _last_ctrl_pressed) {
		_thd.dirty = 0xff;
		_last_ctrl_pressed = _ctrl_pressed;
	}

	if (_thd.dirty & 1) {
		_thd.dirty &= ~1;

		if (_thd.drawstyle == HT_RECT) {
			FindStationsAroundSelection<T>();
		}
	}
}

void CheckRedrawRailWaypointCoverage(const Window *)
{
	CheckRedrawWaypointCoverage<RailWaypointTypeFilter>();
}

void CheckRedrawRoadWaypointCoverage(const Window *)
{
	CheckRedrawWaypointCoverage<RoadWaypointTypeFilter>();
}

/**
 * Draw small boxes of cargo amount and ratings data at the given
 * coordinates. If amount exceeds 576 units, it is shown 'full', same
 * goes for the rating: at above 90% orso (224) it is also 'full'
 *
 * @param left   left most coordinate to draw the box at
 * @param right  right most coordinate to draw the box at
 * @param y      coordinate to draw the box at
 * @param cargo  Cargo type
 * @param amount Cargo amount
 * @param rating ratings data for that particular cargo
 */
static void StationsWndShowStationRating(int left, int right, int y, CargoType cargo, uint amount, uint8_t rating)
{
	static const uint units_full  = 576; ///< number of units to show station as 'full'
	static const uint rating_full = 224; ///< rating needed so it is shown as 'full'

	const CargoSpec *cs = CargoSpec::Get(cargo);
	if (!cs->IsValid()) return;

	int padding = ScaleGUITrad(1);
	int width = right - left;
	int colour = cs->rating_colour;
	TextColour tc = GetContrastColour(colour);
	uint w = std::min(amount + 5, units_full) * width / units_full;

	int height = GetCharacterHeight(FS_SMALL) + padding - 1;

	if (amount > 30) {
		/* Draw total cargo (limited) on station */
		GfxFillRect(left, y, left + w - 1, y + height, colour);
	} else {
		/* Draw a (scaled) one pixel-wide bar of additional cargo meter, useful
		 * for stations with only a small amount (<=30) */
		uint rest = ScaleGUITrad(amount) / 5;
		if (rest != 0) {
			GfxFillRect(left, y + height - rest, left + padding - 1, y + height, colour);
		}
	}

	DrawString(left + padding, right, y, cs->abbrev, tc, SA_CENTER, false, FS_SMALL);

	/* Draw green/red ratings bar (fits under the waiting bar) */
	y += height + padding + 1;
	GfxFillRect(left + padding, y, right - padding - 1, y + padding - 1, PC_RED);
	w = std::min<uint>(rating, rating_full) * (width - padding - padding) / rating_full;
	if (w != 0) GfxFillRect(left + padding, y, left + w - 1, y + padding - 1, PC_GREEN);
}

typedef GUIList<const Station*, const CargoTypes &> GUIStationList;

/**
 * The list of stations per company.
 */
class CompanyStationsWindow : public Window
{
protected:
	/* Runtime saved values */
	struct FilterState {
		Listing last_sorting;
		StationFacilities facilities; ///< types of stations of interest
		bool include_no_rating; ///< Whether we should include stations with no cargo rating.
		CargoTypes cargoes; ///< bitmap of cargo types to include
	};

	static inline FilterState initial_state = {
		{false, 0},
		{StationFacility::Train, StationFacility::TruckStop, StationFacility::BusStop, StationFacility::Airport, StationFacility::Dock},
		true,
		ALL_CARGOTYPES,
	};

	/* Constants for sorting stations */
	static inline const StringID sorter_names[] = {
		STR_SORT_BY_NAME,
		STR_SORT_BY_FACILITY,
		STR_SORT_BY_WAITING_TOTAL,
		STR_SORT_BY_WAITING_AVAILABLE,
		STR_SORT_BY_RATING_MAX,
		STR_SORT_BY_RATING_MIN,
	};
	static const std::initializer_list<GUIStationList::SortFunction * const> sorter_funcs;

	FilterState filter{};
	GUIStationList stations{filter.cargoes};
	Scrollbar *vscroll = nullptr;
	uint rating_width = 0;
	bool filter_expanded = false;
	std::array<uint16_t, NUM_CARGO> stations_per_cargo_type{}; ///< Number of stations with a rating for each cargo type.
	uint16_t stations_per_cargo_type_no_rating = 0; ///< Number of stations without a rating.

	/**
	 * (Re)Build station list
	 *
	 * @param owner company whose stations are to be in list
	 */
	void BuildStationsList(const Owner owner)
	{
		if (!this->stations.NeedRebuild()) return;

		Debug(misc, 3, "Building station list for company {}", owner);

		this->stations.clear();
		this->stations_per_cargo_type.fill(0);
		this->stations_per_cargo_type_no_rating = 0;

		for (const Station *st : Station::Iterate()) {
			if (this->filter.facilities.Any(st->facilities)) { // only stations with selected facilities
				if (st->owner == owner || (st->owner == OWNER_NONE && HasStationInUse(st->index, true, owner))) {
					bool has_rating = false;
					/* Add to the station/cargo counts. */
					for (CargoType cargo = 0; cargo < NUM_CARGO; ++cargo) {
						if (st->goods[cargo].HasRating()) this->stations_per_cargo_type[cargo]++;
					}
					for (CargoType cargo = 0; cargo < NUM_CARGO; ++cargo) {
						if (st->goods[cargo].HasRating()) {
							has_rating = true;
							if (HasBit(this->filter.cargoes, cargo)) {
								this->stations.push_back(st);
								break;
							}
						}
					}
					/* Stations with no cargo rating. */
					if (!has_rating) {
						if (this->filter.include_no_rating) this->stations.push_back(st);
						this->stations_per_cargo_type_no_rating++;
					}
				}
			}
		}

		this->stations.RebuildDone();

		this->vscroll->SetCount(this->stations.size()); // Update the scrollbar
	}

	/** Sort stations by their name */
	static bool StationNameSorter(const Station * const &a, const Station * const &b, const CargoTypes &)
	{
		int r = StrNaturalCompare(a->GetCachedName(), b->GetCachedName()); // Sort by name (natural sorting).
		if (r == 0) return a->index < b->index;
		return r < 0;
	}

	/** Sort stations by their type */
	static bool StationTypeSorter(const Station * const &a, const Station * const &b, const CargoTypes &)
	{
		return a->facilities < b->facilities;
	}

	/** Sort stations by their waiting cargo */
	static bool StationWaitingTotalSorter(const Station * const &a, const Station * const &b, const CargoTypes &cargo_filter)
	{
		int diff = 0;

		for (CargoType cargo : SetCargoBitIterator(cargo_filter)) {
			diff += (a->goods[cargo].HasData() ? a->goods[cargo].GetData().cargo.TotalCount() : 0) - (b->goods[cargo].HasData() ? b->goods[cargo].GetData().cargo.TotalCount() : 0);
		}

		return diff < 0;
	}

	/** Sort stations by their available waiting cargo */
	static bool StationWaitingAvailableSorter(const Station * const &a, const Station * const &b, const CargoTypes &cargo_filter)
	{
		int diff = 0;

		for (CargoType cargo : SetCargoBitIterator(cargo_filter)) {
			diff += (a->goods[cargo].HasData() ? a->goods[cargo].GetData().cargo.AvailableCount() : 0) - (b->goods[cargo].HasData() ? b->goods[cargo].GetData().cargo.AvailableCount() : 0);
		}

		return diff < 0;
	}

	/** Sort stations by their rating */
	static bool StationRatingMaxSorter(const Station * const &a, const Station * const &b, const CargoTypes &cargo_filter)
	{
		uint8_t maxr1 = 0;
		uint8_t maxr2 = 0;

		for (CargoType cargo : SetCargoBitIterator(cargo_filter)) {
			if (a->goods[cargo].HasRating()) maxr1 = std::max(maxr1, a->goods[cargo].rating);
			if (b->goods[cargo].HasRating()) maxr2 = std::max(maxr2, b->goods[cargo].rating);
		}

		return maxr1 < maxr2;
	}

	/** Sort stations by their rating */
	static bool StationRatingMinSorter(const Station * const &a, const Station * const &b, const CargoTypes &cargo_filter)
	{
		uint8_t minr1 = 255;
		uint8_t minr2 = 255;

		for (CargoType cargo : SetCargoBitIterator(cargo_filter)) {
			if (a->goods[cargo].HasRating()) minr1 = std::min(minr1, a->goods[cargo].rating);
			if (b->goods[cargo].HasRating()) minr2 = std::min(minr2, b->goods[cargo].rating);
		}

		return minr1 > minr2;
	}

	/** Sort the stations list */
	void SortStationsList()
	{
		if (!this->stations.Sort()) return;

		/* Set the modified widget dirty */
		this->SetWidgetDirty(WID_STL_LIST);
	}

public:
	CompanyStationsWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc)
	{
		/* Load initial filter state. */
		this->filter = CompanyStationsWindow::initial_state;
		if (this->filter.cargoes == ALL_CARGOTYPES) this->filter.cargoes = _cargo_mask;

		this->stations.SetListing(this->filter.last_sorting);
		this->stations.SetSortFuncs(CompanyStationsWindow::sorter_funcs);
		this->stations.ForceRebuild();
		this->stations.NeedResort();
		this->SortStationsList();

		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_STL_SCROLLBAR);
		this->FinishInitNested(window_number);
		this->owner = this->window_number;

		if (this->filter.cargoes == ALL_CARGOTYPES) this->filter.cargoes = _cargo_mask;

		for (uint i = 0; i < 5; i++) {
			if (HasBit(this->filter.facilities.base(), i)) this->LowerWidget(i + WID_STL_TRAIN);
		}

		this->GetWidget<NWidgetCore>(WID_STL_SORTDROPBTN)->SetString(CompanyStationsWindow::sorter_names[this->stations.SortType()]);
	}

	~CompanyStationsWindow()
	{
		/* Save filter state. */
		this->filter.last_sorting = this->stations.GetListing();
		CompanyStationsWindow::initial_state = this->filter;
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_STL_SORTBY: {
				Dimension d = GetStringBoundingBox(this->GetWidget<NWidgetCore>(widget)->GetString());
				d.width += padding.width + Window::SortButtonWidth() * 2; // Doubled since the string is centred and it also looks better.
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}

			case WID_STL_SORTDROPBTN: {
				Dimension d = GetStringListBoundingBox(CompanyStationsWindow::sorter_names);
				d.width += padding.width;
				d.height += padding.height;
				size = maxdim(size, d);
				break;
			}

			case WID_STL_LIST:
				fill.height = resize.height = std::max(GetCharacterHeight(FS_NORMAL), GetCharacterHeight(FS_SMALL) + ScaleGUITrad(3));
				size.height = padding.height + 5 * resize.height;

				/* Determine appropriate width for mini station rating graph */
				this->rating_width = 0;
				for (const CargoSpec *cs : _sorted_standard_cargo_specs) {
					this->rating_width = std::max(this->rating_width, GetStringBoundingBox(cs->abbrev, FS_SMALL).width);
				}
				/* Approximately match original 16 pixel wide rating bars by multiplying string width by 1.6 */
				this->rating_width = this->rating_width * 16 / 10;
				break;
		}
	}

	void OnPaint() override
	{
		this->BuildStationsList(this->window_number);
		this->SortStationsList();

		this->DrawWidgets();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_STL_SORTBY:
				/* draw arrow pointing up/down for ascending/descending sorting */
				this->DrawSortButtonState(WID_STL_SORTBY, this->stations.IsDescSortOrder() ? SBS_DOWN : SBS_UP);
				break;

			case WID_STL_LIST: {
				bool rtl = _current_text_dir == TD_RTL;
				Rect tr = r.Shrink(WidgetDimensions::scaled.framerect);
				uint line_height = this->GetWidget<NWidgetBase>(widget)->resize_y;
				/* Spacing between station name and first rating graph. */
				int text_spacing = WidgetDimensions::scaled.hsep_wide;
				/* Spacing between additional rating graphs. */
				int rating_spacing = WidgetDimensions::scaled.hsep_normal;

				auto [first, last] = this->vscroll->GetVisibleRangeIterators(this->stations);
				for (auto it = first; it != last; ++it) {
					const Station *st = *it;
					assert(st->xy != INVALID_TILE);

					/* Do not do the complex check HasStationInUse here, it may be even false
					 * when the order had been removed and the station list hasn't been removed yet */
					assert(st->owner == owner || st->owner == OWNER_NONE);

					int x = DrawString(tr.left, tr.right, tr.top + (line_height - GetCharacterHeight(FS_NORMAL)) / 2, GetString(STR_STATION_LIST_STATION, st->index, st->facilities));
					x += rtl ? -text_spacing : text_spacing;

					/* show cargo waiting and station ratings */
					for (const CargoSpec *cs : _sorted_standard_cargo_specs) {
						CargoType cargo_type = cs->Index();
						if (st->goods[cargo_type].HasRating()) {
							/* For RTL we work in exactly the opposite direction. So
							 * decrement the space needed first, then draw to the left
							 * instead of drawing to the left and then incrementing
							 * the space. */
							if (rtl) {
								x -= rating_width + rating_spacing;
								if (x < tr.left) break;
							}
							StationsWndShowStationRating(x, x + rating_width, tr.top, cargo_type, st->goods[cargo_type].HasData() ? st->goods[cargo_type].GetData().cargo.TotalCount() : 0, st->goods[cargo_type].rating);
							if (!rtl) {
								x += rating_width + rating_spacing;
								if (x > tr.right) break;
							}
						}
					}
					tr.top += line_height;
				}

				if (this->vscroll->GetCount() == 0) { // company has no stations
					DrawString(tr.left, tr.right, tr.top + (line_height - GetCharacterHeight(FS_NORMAL)) / 2, STR_STATION_LIST_NONE);
					return;
				}
				break;
			}
		}
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget == WID_STL_CAPTION) {
			return GetString(STR_STATION_LIST_CAPTION, this->window_number, this->vscroll->GetCount());
		}

		if (widget == WID_STL_CARGODROPDOWN) {
			if (this->filter.cargoes == 0) return GetString(this->filter.include_no_rating ? STR_STATION_LIST_CARGO_FILTER_ONLY_NO_RATING : STR_STATION_LIST_CARGO_FILTER_NO_CARGO_TYPES);
			if (this->filter.cargoes == _cargo_mask) return GetString(this->filter.include_no_rating ? STR_STATION_LIST_CARGO_FILTER_ALL_AND_NO_RATING : STR_CARGO_TYPE_FILTER_ALL);
			if (CountBits(this->filter.cargoes) == 1 && !this->filter.include_no_rating) return GetString(CargoSpec::Get(FindFirstBit(this->filter.cargoes))->name);
			return GetString(STR_STATION_LIST_CARGO_FILTER_MULTIPLE);
		}

		return this->Window::GetWidgetString(widget, stringid);
	}

	DropDownList BuildCargoDropDownList(bool expanded) const
	{
		/* Define a custom item consisting of check mark, count string, icon and name string. */
		using DropDownListCargoItem = DropDownCheck<DropDownString<DropDownListIconItem, FS_SMALL, true>>;

		DropDownList list;
		list.push_back(MakeDropDownListStringItem(STR_STATION_LIST_CARGO_FILTER_SELECT_ALL, CargoFilterCriteria::CF_SELECT_ALL));
		list.push_back(MakeDropDownListDividerItem());

		bool any_hidden = false;

		uint16_t count = this->stations_per_cargo_type_no_rating;
		if (count == 0 && !expanded) {
			any_hidden = true;
		} else {
			list.push_back(std::make_unique<DropDownString<DropDownListCheckedItem, FS_SMALL, true>>(fmt::format("{}", count), 0, this->filter.include_no_rating, GetString(STR_STATION_LIST_CARGO_FILTER_NO_RATING), CargoFilterCriteria::CF_NO_RATING, false, count == 0));
		}

		Dimension d = GetLargestCargoIconSize();
		for (const CargoSpec *cs : _sorted_cargo_specs) {
			count = this->stations_per_cargo_type[cs->Index()];
			if (count == 0 && !expanded) {
				any_hidden = true;
			} else {
				list.push_back(std::make_unique<DropDownListCargoItem>(HasBit(this->filter.cargoes, cs->Index()), fmt::format("{}", count), d, cs->GetCargoIcon(), PAL_NONE, GetString(cs->name), cs->Index(), false, count == 0));
			}
		}

		if (!expanded && any_hidden) {
			if (list.size() > 2) list.push_back(MakeDropDownListDividerItem());
			list.push_back(MakeDropDownListStringItem(STR_STATION_LIST_CARGO_FILTER_EXPAND, CargoFilterCriteria::CF_EXPAND_LIST));
		}

		return list;
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_STL_LIST: {
				auto it = this->vscroll->GetScrolledItemFromWidget(this->stations, pt.y, this, WID_STL_LIST, WidgetDimensions::scaled.framerect.top);
				if (it == this->stations.end()) return; // click out of list bound

				const Station *st = *it;
				/* do not check HasStationInUse - it is slow and may be invalid */
				assert(st->owner == this->window_number || st->owner == OWNER_NONE);

				if (_ctrl_pressed) {
					ShowExtraViewportWindow(st->xy);
				} else {
					ScrollMainWindowToTile(st->xy);
				}
				break;
			}

			case WID_STL_TRAIN:
			case WID_STL_TRUCK:
			case WID_STL_BUS:
			case WID_STL_AIRPLANE:
			case WID_STL_SHIP:
				if (_ctrl_pressed) {
					this->filter.facilities.Flip(static_cast<StationFacility>(widget - WID_STL_TRAIN));
					this->ToggleWidgetLoweredState(widget);
				} else {
					for (StationFacility i : this->filter.facilities) {
						this->RaiseWidget(to_underlying(i) + WID_STL_TRAIN);
					}
					this->filter.facilities = {};
					this->filter.facilities.Set(static_cast<StationFacility>(widget - WID_STL_TRAIN));
					this->LowerWidget(widget);
				}
				this->stations.ForceRebuild();
				this->SetDirty();
				break;

			case WID_STL_FACILALL:
				for (WidgetID i = WID_STL_TRAIN; i <= WID_STL_SHIP; i++) {
					this->LowerWidget(i);
				}

				this->filter.facilities = {StationFacility::Train, StationFacility::TruckStop, StationFacility::BusStop, StationFacility::Airport, StationFacility::Dock};
				this->stations.ForceRebuild();
				this->SetDirty();
				break;

			case WID_STL_SORTBY: // flip sorting method asc/desc
				this->stations.ToggleSortOrder();
				this->SetDirty();
				break;

			case WID_STL_SORTDROPBTN: // select sorting criteria dropdown menu
				ShowDropDownMenu(this, CompanyStationsWindow::sorter_names, this->stations.SortType(), WID_STL_SORTDROPBTN, 0, 0);
				break;

			case WID_STL_CARGODROPDOWN:
				this->filter_expanded = false;
				ShowDropDownList(this, this->BuildCargoDropDownList(this->filter_expanded), -1, widget, 0, false, true);
				break;
		}
	}

	void OnDropdownSelect(WidgetID widget, int index, int) override
	{
		if (widget == WID_STL_SORTDROPBTN) {
			if (this->stations.SortType() != index) {
				this->stations.SetSortType(index);

				/* Display the current sort variant */
				this->GetWidget<NWidgetCore>(WID_STL_SORTDROPBTN)->SetString(CompanyStationsWindow::sorter_names[this->stations.SortType()]);

				this->SetDirty();
			}
		}

		if (widget == WID_STL_CARGODROPDOWN) {
			FilterState oldstate = this->filter;

			if (index >= 0 && index < NUM_CARGO) {
				if (_ctrl_pressed) {
					ToggleBit(this->filter.cargoes, index);
				} else {
					this->filter.cargoes = 1ULL << index;
					this->filter.include_no_rating = false;
				}
			} else if (index == CargoFilterCriteria::CF_NO_RATING) {
				if (_ctrl_pressed) {
					this->filter.include_no_rating = !this->filter.include_no_rating;
				} else {
					this->filter.include_no_rating = true;
					this->filter.cargoes = 0;
				}
			} else if (index == CargoFilterCriteria::CF_SELECT_ALL) {
				this->filter.cargoes = _cargo_mask;
				this->filter.include_no_rating = true;
			} else if (index == CargoFilterCriteria::CF_EXPAND_LIST) {
				this->filter_expanded = true;
				ReplaceDropDownList(this, this->BuildCargoDropDownList(this->filter_expanded));
				return;
			}

			if (oldstate.cargoes != this->filter.cargoes || oldstate.include_no_rating != this->filter.include_no_rating) {
				this->stations.ForceRebuild();
				this->SetDirty();

				/* Only refresh the list if it's changed. */
				if (_ctrl_pressed) ReplaceDropDownList(this, this->BuildCargoDropDownList(this->filter_expanded));
			}

			/* Always close the list if ctrl is not pressed. */
			if (!_ctrl_pressed) this->CloseChildWindows(WC_DROPDOWN_MENU);
		}
	}

	void OnGameTick() override
	{
		if (this->stations.NeedResort()) {
			Debug(misc, 3, "Periodic rebuild station list company {}", static_cast<int>(this->window_number));
			this->SetDirty();
		}
	}

	void OnResize() override
	{
		this->vscroll->SetCapacityFromWidget(this, WID_STL_LIST, WidgetDimensions::scaled.framerect.Vertical());
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Information about the changed data.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (data == 0) {
			/* This needs to be done in command-scope to enforce rebuilding before resorting invalid data */
			this->stations.ForceRebuild();
		} else {
			this->stations.ForceResort();
		}
	}
};

/* Available station sorting functions */
const std::initializer_list<GUIStationList::SortFunction * const> CompanyStationsWindow::sorter_funcs = {
	&StationNameSorter,
	&StationTypeSorter,
	&StationWaitingTotalSorter,
	&StationWaitingAvailableSorter,
	&StationRatingMaxSorter,
	&StationRatingMinSorter
};

static constexpr NWidgetPart _nested_company_stations_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_STL_CAPTION),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_DEFSIZEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_STL_TRAIN), SetAspect(WidgetDimensions::ASPECT_VEHICLE_ICON), SetStringTip(STR_TRAIN, STR_STATION_LIST_USE_CTRL_TO_SELECT_MORE_TOOLTIP), SetFill(0, 1),
		NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_STL_TRUCK), SetAspect(WidgetDimensions::ASPECT_VEHICLE_ICON), SetStringTip(STR_LORRY, STR_STATION_LIST_USE_CTRL_TO_SELECT_MORE_TOOLTIP), SetFill(0, 1),
		NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_STL_BUS), SetAspect(WidgetDimensions::ASPECT_VEHICLE_ICON), SetStringTip(STR_BUS, STR_STATION_LIST_USE_CTRL_TO_SELECT_MORE_TOOLTIP), SetFill(0, 1),
		NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_STL_SHIP), SetAspect(WidgetDimensions::ASPECT_VEHICLE_ICON), SetStringTip(STR_SHIP, STR_STATION_LIST_USE_CTRL_TO_SELECT_MORE_TOOLTIP), SetFill(0, 1),
		NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_STL_AIRPLANE), SetAspect(WidgetDimensions::ASPECT_VEHICLE_ICON), SetStringTip(STR_PLANE, STR_STATION_LIST_USE_CTRL_TO_SELECT_MORE_TOOLTIP), SetFill(0, 1),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_STL_FACILALL), SetAspect(WidgetDimensions::ASPECT_VEHICLE_ICON), SetStringTip(STR_ABBREV_ALL, STR_STATION_LIST_SELECT_ALL_FACILITIES_TOOLTIP), SetTextStyle(TC_BLACK, FS_SMALL), SetFill(0, 1),
		NWidget(WWT_PANEL, COLOUR_GREY), SetMinimalSize(5, 0), SetFill(0, 1), EndContainer(),
		NWidget(WWT_DROPDOWN, COLOUR_GREY, WID_STL_CARGODROPDOWN), SetFill(1, 0), SetToolTip(STR_STATION_LIST_USE_CTRL_TO_SELECT_MORE_TOOLTIP),
		NWidget(WWT_PANEL, COLOUR_GREY), SetResize(1, 0), SetFill(1, 1), EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_STL_SORTBY), SetMinimalSize(81, 12), SetStringTip(STR_BUTTON_SORT_BY, STR_TOOLTIP_SORT_ORDER),
		NWidget(WWT_DROPDOWN, COLOUR_GREY, WID_STL_SORTDROPBTN), SetMinimalSize(163, 12), SetStringTip(STR_SORT_BY_NAME, STR_TOOLTIP_SORT_CRITERIA), // widget_data gets overwritten.
		NWidget(WWT_PANEL, COLOUR_GREY), SetResize(1, 0), SetFill(1, 1), EndContainer(),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_GREY, WID_STL_LIST), SetMinimalSize(346, 125), SetResize(1, 10), SetToolTip(STR_STATION_LIST_TOOLTIP), SetScrollbar(WID_STL_SCROLLBAR), EndContainer(),
		NWidget(NWID_VERTICAL),
			NWidget(NWID_VSCROLLBAR, COLOUR_GREY, WID_STL_SCROLLBAR),
			NWidget(WWT_RESIZEBOX, COLOUR_GREY),
		EndContainer(),
	EndContainer(),
};

static WindowDesc _company_stations_desc(
	WDP_AUTO, "list_stations", 358, 162,
	WC_STATION_LIST, WC_NONE,
	{},
	_nested_company_stations_widgets
);

/**
 * Opens window with list of company's stations
 *
 * @param company whose stations' list show
 */
void ShowCompanyStations(CompanyID company)
{
	if (!Company::IsValidID(company)) return;

	AllocateWindowDescFront<CompanyStationsWindow>(_company_stations_desc, company);
}

static constexpr NWidgetPart _nested_station_view_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_GREY),
		NWidget(WWT_PUSHIMGBTN, COLOUR_GREY, WID_SV_RENAME), SetAspect(WidgetDimensions::ASPECT_RENAME), SetSpriteTip(SPR_RENAME, STR_STATION_VIEW_RENAME_TOOLTIP),
		NWidget(WWT_CAPTION, COLOUR_GREY, WID_SV_CAPTION),
		NWidget(WWT_PUSHIMGBTN, COLOUR_GREY, WID_SV_LOCATION), SetAspect(WidgetDimensions::ASPECT_LOCATION), SetSpriteTip(SPR_GOTO_LOCATION, STR_STATION_VIEW_CENTER_TOOLTIP),
		NWidget(WWT_SHADEBOX, COLOUR_GREY),
		NWidget(WWT_DEFSIZEBOX, COLOUR_GREY),
		NWidget(WWT_STICKYBOX, COLOUR_GREY),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_SV_GROUP), SetMinimalSize(81, 12), SetFill(1, 1), SetStringTip(STR_STATION_VIEW_GROUP),
		NWidget(WWT_DROPDOWN, COLOUR_GREY, WID_SV_GROUP_BY), SetMinimalSize(168, 12), SetResize(1, 0), SetFill(0, 1), SetToolTip(STR_TOOLTIP_GROUP_ORDER),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_SV_SORT_ORDER), SetMinimalSize(81, 12), SetFill(1, 1), SetStringTip(STR_BUTTON_SORT_BY, STR_TOOLTIP_SORT_ORDER),
		NWidget(WWT_DROPDOWN, COLOUR_GREY, WID_SV_SORT_BY), SetMinimalSize(168, 12), SetResize(1, 0), SetFill(0, 1), SetToolTip(STR_TOOLTIP_SORT_CRITERIA),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_GREY, WID_SV_WAITING), SetMinimalSize(237, 44), SetResize(1, 10), SetScrollbar(WID_SV_SCROLLBAR), EndContainer(),
		NWidget(NWID_VSCROLLBAR, COLOUR_GREY, WID_SV_SCROLLBAR),
	EndContainer(),
	NWidget(WWT_PANEL, COLOUR_GREY, WID_SV_ACCEPT_RATING_LIST), SetMinimalSize(249, 23), SetResize(1, 0), EndContainer(),
	NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_SV_ACCEPTS_RATINGS), SetMinimalSize(46, 12), SetResize(1, 0), SetFill(1, 1),
				SetStringTip(STR_STATION_VIEW_RATINGS_BUTTON, STR_STATION_VIEW_RATINGS_TOOLTIP),
		NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_SV_CLOSE_AIRPORT), SetMinimalSize(45, 12), SetResize(1, 0), SetFill(1, 1),
				SetStringTip(STR_STATION_VIEW_CLOSE_AIRPORT, STR_STATION_VIEW_CLOSE_AIRPORT_TOOLTIP),
		NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_SV_CATCHMENT), SetMinimalSize(45, 12), SetResize(1, 0), SetFill(1, 1), SetStringTip(STR_BUTTON_CATCHMENT, STR_TOOLTIP_CATCHMENT),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_SV_TRAINS), SetAspect(WidgetDimensions::ASPECT_VEHICLE_ICON), SetFill(0, 1), SetStringTip(STR_TRAIN, STR_STATION_VIEW_SCHEDULED_TRAINS_TOOLTIP),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_SV_ROADVEHS), SetAspect(WidgetDimensions::ASPECT_VEHICLE_ICON), SetFill(0, 1), SetStringTip(STR_LORRY, STR_STATION_VIEW_SCHEDULED_ROAD_VEHICLES_TOOLTIP),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_SV_SHIPS), SetAspect(WidgetDimensions::ASPECT_VEHICLE_ICON), SetFill(0, 1), SetStringTip(STR_SHIP, STR_STATION_VIEW_SCHEDULED_SHIPS_TOOLTIP),
		NWidget(WWT_PUSHTXTBTN, COLOUR_GREY, WID_SV_PLANES),  SetAspect(WidgetDimensions::ASPECT_VEHICLE_ICON), SetFill(0, 1), SetStringTip(STR_PLANE, STR_STATION_VIEW_SCHEDULED_AIRCRAFT_TOOLTIP),
		NWidget(WWT_RESIZEBOX, COLOUR_GREY),
	EndContainer(),
};

/**
 * Draws icons of waiting cargo in the StationView window
 *
 * @param cargo type of cargo
 * @param waiting number of waiting units
 * @param left  left most coordinate to draw on
 * @param right right most coordinate to draw on
 * @param y y coordinate
 */
static void DrawCargoIcons(CargoType cargo, uint waiting, int left, int right, int y)
{
	int width = ScaleSpriteTrad(10);
	uint num = std::min<uint>((waiting + (width / 2)) / width, (right - left) / width); // maximum is width / 10 icons so it won't overflow
	if (num == 0) return;

	SpriteID sprite = CargoSpec::Get(cargo)->GetCargoIcon();

	int x = _current_text_dir == TD_RTL ? left : right - num * width;
	do {
		DrawSprite(sprite, PAL_NONE, x, y);
		x += width;
	} while (--num);
}

enum SortOrder : uint8_t {
	SO_DESCENDING,
	SO_ASCENDING
};

class CargoDataEntry;

enum class CargoSortType : uint8_t {
	AsGrouping,    ///< by the same principle the entries are being grouped
	Count,         ///< by amount of cargo
	StationString, ///< by station name
	StationID,     ///< by station id
	CargoType,     ///< by cargo type
};

class CargoSorter {
public:
	using is_transparent = void;
	CargoSorter(CargoSortType t = CargoSortType::StationID, SortOrder o = SO_ASCENDING) : type(t), order(o) {}
	CargoSortType GetSortType() {return this->type;}
	bool operator()(const CargoDataEntry &cd1, const CargoDataEntry &cd2) const;
	bool operator()(const CargoDataEntry &cd1, const std::unique_ptr<CargoDataEntry> &cd2) const { return this->operator()(cd1, *cd2); }
	bool operator()(const std::unique_ptr<CargoDataEntry> &cd1, const CargoDataEntry &cd2) const { return this->operator()(*cd1, cd2); }
	bool operator()(const std::unique_ptr<CargoDataEntry> &cd1, const std::unique_ptr<CargoDataEntry> &cd2) const { return this->operator()(*cd1, *cd2); }

private:
	CargoSortType type;
	SortOrder order;

	template <class Tid>
	bool SortId(Tid st1, Tid st2) const;
	bool SortCount(const CargoDataEntry &cd1, const CargoDataEntry &cd2) const;
	bool SortStation(StationID st1, StationID st2) const;
};

typedef std::set<std::unique_ptr<CargoDataEntry>, CargoSorter> CargoDataSet;

/**
 * A cargo data entry representing one possible row in the station view window's
 * top part. Cargo data entries form a tree where each entry can have several
 * children. Parents keep track of the sums of their childrens' cargo counts.
 */
class CargoDataEntry {
public:
	CargoDataEntry();
	~CargoDataEntry();

	/**
	 * Insert a new child or retrieve an existing child using a station ID as ID.
	 * @param station ID of the station for which an entry shall be created or retrieved
	 * @return a child entry associated with the given station.
	 */
	CargoDataEntry &InsertOrRetrieve(StationID station)
	{
		return this->InsertOrRetrieve<StationID>(station);
	}

	/**
	 * Insert a new child or retrieve an existing child using a cargo type as ID.
	 * @param cargo type of the cargo for which an entry shall be created or retrieved
	 * @return a child entry associated with the given cargo.
	 */
	CargoDataEntry &InsertOrRetrieve(CargoType cargo)
	{
		return this->InsertOrRetrieve<CargoType>(cargo);
	}

	void Update(uint count);

	/**
	 * Remove a child associated with the given station.
	 * @param station ID of the station for which the child should be removed.
	 */
	void Remove(StationID station)
	{
		CargoDataEntry t(station);
		this->Remove(t);
	}

	/**
	 * Remove a child associated with the given cargo.
	 * @param cargo type of the cargo for which the child should be removed.
	 */
	void Remove(CargoType cargo)
	{
		CargoDataEntry t(cargo);
		this->Remove(t);
	}

	/**
	 * Retrieve a child for the given station. Return nullptr if it doesn't exist.
	 * @param station ID of the station the child we're looking for is associated with.
	 * @return a child entry for the given station or nullptr.
	 */
	CargoDataEntry *Retrieve(StationID station) const
	{
		CargoDataEntry t(station);
		return this->Retrieve(this->children->find(t));
	}

	/**
	 * Retrieve a child for the given cargo. Return nullptr if it doesn't exist.
	 * @param cargo type of the cargo the child we're looking for is associated with.
	 * @return a child entry for the given cargo or nullptr.
	 */
	CargoDataEntry *Retrieve(CargoType cargo) const
	{
		CargoDataEntry t(cargo);
		return this->Retrieve(this->children->find(t));
	}

	void Resort(CargoSortType type, SortOrder order);

	/**
	 * Get the station ID for this entry.
	 */
	StationID GetStation() const { return this->station; }

	/**
	 * Get the cargo type for this entry.
	 */
	CargoType GetCargo() const { return this->cargo; }

	/**
	 * Get the cargo count for this entry.
	 */
	uint GetCount() const { return this->count; }

	/**
	 * Get the parent entry for this entry.
	 */
	CargoDataEntry *GetParent() const { return this->parent; }

	/**
	 * Get the number of children for this entry.
	 */
	uint GetNumChildren() const { return this->num_children; }

	/**
	 * Get an iterator pointing to the begin of the set of children.
	 */
	CargoDataSet::iterator Begin() const { return this->children->begin(); }

	/**
	 * Get an iterator pointing to the end of the set of children.
	 */
	CargoDataSet::iterator End() const { return this->children->end(); }

	/**
	 * Has this entry transfers.
	 */
	bool HasTransfers() const { return this->transfers; }

	/**
	 * Set the transfers state.
	 */
	void SetTransfers(bool value) { this->transfers = value; }

	void Clear();

	CargoDataEntry(StationID station, uint count, CargoDataEntry *parent);
	CargoDataEntry(CargoType cargo, uint count, CargoDataEntry *parent);
	CargoDataEntry(StationID station);
	CargoDataEntry(CargoType cargo);

private:
	CargoDataEntry *Retrieve(CargoDataSet::iterator i) const;

	template <class Tid>
	CargoDataEntry &InsertOrRetrieve(Tid s);

	void Remove(CargoDataEntry &entry);
	void IncrementSize();

	CargoDataEntry *parent;   ///< the parent of this entry.
	const union {
		StationID station;    ///< ID of the station this entry is associated with.
		struct {
			CargoType cargo;    ///< ID of the cargo this entry is associated with.
			bool transfers;   ///< If there are transfers for this cargo.
		};
	};
	uint num_children;        ///< the number of subentries belonging to this entry.
	uint count;               ///< sum of counts of all children or amount of cargo for this entry.
	std::unique_ptr<CargoDataSet> children;   ///< the children of this entry.
};

CargoDataEntry::CargoDataEntry() :
	parent(nullptr),
	station(StationID::Invalid()),
	num_children(0),
	count(0),
	children(std::make_unique<CargoDataSet>(CargoSorter(CargoSortType::CargoType)))
{}

CargoDataEntry::CargoDataEntry(CargoType cargo, uint count, CargoDataEntry *parent) :
	parent(parent),
	cargo(cargo),
	num_children(0),
	count(count),
	children(std::make_unique<CargoDataSet>())
{}

CargoDataEntry::CargoDataEntry(StationID station, uint count, CargoDataEntry *parent) :
	parent(parent),
	station(station),
	num_children(0),
	count(count),
	children(std::make_unique<CargoDataSet>())
{}

CargoDataEntry::CargoDataEntry(StationID station) :
	parent(nullptr),
	station(station),
	num_children(0),
	count(0),
	children(nullptr)
{}

CargoDataEntry::CargoDataEntry(CargoType cargo) :
	parent(nullptr),
	cargo(cargo),
	num_children(0),
	count(0),
	children(nullptr)
{}

CargoDataEntry::~CargoDataEntry()
{
	this->Clear();
}

/**
 * Delete all subentries, reset count and num_children and adapt parent's count.
 */
void CargoDataEntry::Clear()
{
	if (this->children != nullptr) this->children->clear();
	if (this->parent != nullptr) this->parent->count -= this->count;
	this->count = 0;
	this->num_children = 0;
}

/**
 * Remove a subentry from this one and delete it.
 * @param child the entry to be removed. This may also be a synthetic entry
 * which only contains the ID of the entry to be removed. In this case child is
 * not deleted.
 */
void CargoDataEntry::Remove(CargoDataEntry &entry)
{
	CargoDataSet::iterator i = this->children->find(entry);
	if (i != this->children->end()) this->children->erase(i);
}

/**
 * Retrieve a subentry or insert it if it doesn't exist, yet.
 * @tparam ID type of ID: either StationID or CargoType
 * @param child_id ID of the child to be inserted or retrieved.
 * @return the new or retrieved subentry
 */
template <class Tid>
CargoDataEntry &CargoDataEntry::InsertOrRetrieve(Tid child_id)
{
	CargoDataEntry tmp(child_id);
	CargoDataSet::iterator i = this->children->find(tmp);
	if (i == this->children->end()) {
		IncrementSize();
		return **(this->children->insert(std::make_unique<CargoDataEntry>(child_id, 0, this)).first);
	} else {
		assert(this->children->value_comp().GetSortType() != CargoSortType::Count);
		return **i;
	}
}

/**
 * Update the count for this entry and propagate the change to the parent entry
 * if there is one.
 * @param count the amount to be added to this entry
 */
void CargoDataEntry::Update(uint count)
{
	this->count += count;
	if (this->parent != nullptr) this->parent->Update(count);
}

/**
 * Increment
 */
void CargoDataEntry::IncrementSize()
{
	 ++this->num_children;
	 if (this->parent != nullptr) this->parent->IncrementSize();
}

void CargoDataEntry::Resort(CargoSortType type, SortOrder order)
{
	auto new_children = std::make_unique<CargoDataSet>(CargoSorter(type, order));
	new_children->merge(*this->children);
	this->children = std::move(new_children);
}

CargoDataEntry *CargoDataEntry::Retrieve(CargoDataSet::iterator i) const
{
	if (i == this->children->end()) {
		return nullptr;
	} else {
		assert(this->children->value_comp().GetSortType() != CargoSortType::Count);
		return i->get();
	}
}

bool CargoSorter::operator()(const CargoDataEntry &cd1, const CargoDataEntry &cd2) const
{
	switch (this->type) {
		case CargoSortType::StationID:
			return this->SortId<StationID>(cd1.GetStation(), cd2.GetStation());
		case CargoSortType::CargoType:
			return this->SortId<CargoType>(cd1.GetCargo(), cd2.GetCargo());
		case CargoSortType::Count:
			return this->SortCount(cd1, cd2);
		case CargoSortType::StationString:
			return this->SortStation(cd1.GetStation(), cd2.GetStation());
		default:
			NOT_REACHED();
	}
}

template <class Tid>
bool CargoSorter::SortId(Tid st1, Tid st2) const
{
	return (this->order == SO_ASCENDING) ? st1 < st2 : st2 < st1;
}

bool CargoSorter::SortCount(const CargoDataEntry &cd1, const CargoDataEntry &cd2) const
{
	uint c1 = cd1.GetCount();
	uint c2 = cd2.GetCount();
	if (c1 == c2) {
		return this->SortStation(cd1.GetStation(), cd2.GetStation());
	} else if (this->order == SO_ASCENDING) {
		return c1 < c2;
	} else {
		return c2 < c1;
	}
}

bool CargoSorter::SortStation(StationID st1, StationID st2) const
{
	if (!Station::IsValidID(st1)) {
		return Station::IsValidID(st2) ? this->order == SO_ASCENDING : this->SortId(st1, st2);
	} else if (!Station::IsValidID(st2)) {
		return order == SO_DESCENDING;
	}

	int res = StrNaturalCompare(Station::Get(st1)->GetCachedName(), Station::Get(st2)->GetCachedName()); // Sort by name (natural sorting).
	if (res == 0) {
		return this->SortId(st1, st2);
	} else {
		return (this->order == SO_ASCENDING) ? res < 0 : res > 0;
	}
}

/**
 * The StationView window
 */
struct StationViewWindow : public Window {
	/**
	 * A row being displayed in the cargo view (as opposed to being "hidden" behind a plus sign).
	 */
	struct RowDisplay {
		RowDisplay(CargoDataEntry *f, StationID n) : filter(f), next_station(n) {}
		RowDisplay(CargoDataEntry *f, CargoType n) : filter(f), next_cargo(n) {}

		/**
		 * Parent of the cargo entry belonging to the row.
		 */
		CargoDataEntry *filter;
		union {
			/**
			 * ID of the station belonging to the entry actually displayed if it's to/from/via.
			 */
			StationID next_station;

			/**
			 * ID of the cargo belonging to the entry actually displayed if it's cargo.
			 */
			CargoType next_cargo;
		};
	};

	typedef std::vector<RowDisplay> CargoDataVector;

	static const int NUM_COLUMNS = 4; ///< Number of "columns" in the cargo view: cargo, from, via, to

	/**
	 * Type of data invalidation.
	 */
	enum Invalidation : uint16_t {
		INV_FLOWS = 0x100, ///< The planned flows have been recalculated and everything has to be updated.
		INV_CARGO = 0x200  ///< Some cargo has been added or removed.
	};

	/**
	 * Type of grouping used in each of the "columns".
	 */
	enum Grouping : uint8_t {
		GR_SOURCE,      ///< Group by source of cargo ("from").
		GR_NEXT,        ///< Group by next station ("via").
		GR_DESTINATION, ///< Group by estimated final destination ("to").
		GR_CARGO,       ///< Group by cargo type.
	};

	/**
	 * Display mode of the cargo view.
	 */
	enum Mode : uint8_t {
		MODE_WAITING, ///< Show cargo waiting at the station.
		MODE_PLANNED  ///< Show cargo planned to pass through the station.
	};

	uint expand_shrink_width = 0; ///< The width allocated to the expand/shrink 'button'
	int rating_lines = RATING_LINES; ///< Number of lines in the cargo ratings view.
	int accepts_lines = ACCEPTS_LINES; ///< Number of lines in the accepted cargo view.
	Scrollbar *vscroll = nullptr;

	/* Height of the #WID_SV_ACCEPT_RATING_LIST widget for different views. */
	static constexpr uint RATING_LINES = 13; ///< Height in lines of the cargo ratings view.
	static constexpr uint ACCEPTS_LINES = 3; ///< Height in lines of the accepted cargo view.

	/** Names of the sorting options in the dropdown. */
	static inline const StringID sort_names[] = {
		STR_STATION_VIEW_WAITING_STATION,
		STR_STATION_VIEW_WAITING_AMOUNT,
		STR_STATION_VIEW_PLANNED_STATION,
		STR_STATION_VIEW_PLANNED_AMOUNT,
	};
	/** Names of the grouping options in the dropdown. */
	static inline const StringID group_names[] = {
		STR_STATION_VIEW_GROUP_S_V_D,
		STR_STATION_VIEW_GROUP_S_D_V,
		STR_STATION_VIEW_GROUP_V_S_D,
		STR_STATION_VIEW_GROUP_V_D_S,
		STR_STATION_VIEW_GROUP_D_S_V,
		STR_STATION_VIEW_GROUP_D_V_S,
	};

	/**
	 * Sort types of the different 'columns'.
	 * In fact only CargoSortType::Count and CargoSortType::AsGrouping are active and you can only
	 * sort all the columns in the same way. The other options haven't been
	 * included in the GUI due to lack of space.
	 */
	std::array<CargoSortType, NUM_COLUMNS> sortings{};

	/** Sort order (ascending/descending) for the 'columns'. */
	std::array<SortOrder, NUM_COLUMNS> sort_orders{};

	int scroll_to_row = INT_MAX; ///< If set, scroll the main viewport to the station pointed to by this row.
	int grouping_index = 0; ///< Currently selected entry in the grouping drop down.
	Mode current_mode{}; ///< Currently selected display mode of cargo view.
	std::array<Grouping, NUM_COLUMNS> groupings; ///< Grouping modes for the different columns.

	CargoDataEntry expanded_rows{}; ///< Parent entry of currently expanded rows.
	CargoDataEntry cached_destinations{}; ///< Cache for the flows passing through this station.
	CargoDataVector displayed_rows{}; ///< Parent entry of currently displayed rows (including collapsed ones).

	StationViewWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc)
	{
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_SV_SCROLLBAR);
		/* Nested widget tree creation is done in two steps to ensure that this->GetWidget<NWidgetCore>(WID_SV_ACCEPTS_RATINGS) exists in UpdateWidgetSize(). */
		this->FinishInitNested(window_number);

		this->groupings[0] = GR_CARGO;
		this->sortings[0] = CargoSortType::AsGrouping;
		this->SelectGroupBy(_settings_client.gui.station_gui_group_order);
		this->SelectSortBy(_settings_client.gui.station_gui_sort_by);
		this->sort_orders[0] = SO_ASCENDING;
		this->SelectSortOrder((SortOrder)_settings_client.gui.station_gui_sort_order);
		this->owner = Station::Get(window_number)->owner;
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		CloseWindowById(WC_TRAINS_LIST,   VehicleListIdentifier(VL_STATION_LIST, VEH_TRAIN,    this->owner, this->window_number).ToWindowNumber(), false);
		CloseWindowById(WC_ROADVEH_LIST,  VehicleListIdentifier(VL_STATION_LIST, VEH_ROAD,     this->owner, this->window_number).ToWindowNumber(), false);
		CloseWindowById(WC_SHIPS_LIST,    VehicleListIdentifier(VL_STATION_LIST, VEH_SHIP,     this->owner, this->window_number).ToWindowNumber(), false);
		CloseWindowById(WC_AIRCRAFT_LIST, VehicleListIdentifier(VL_STATION_LIST, VEH_AIRCRAFT, this->owner, this->window_number).ToWindowNumber(), false);

		SetViewportCatchmentStation(Station::Get(this->window_number), false);
		this->Window::Close();
	}

	/**
	 * Show a certain cargo entry characterized by source/next/dest station, cargo type and amount of cargo at the
	 * right place in the cargo view. I.e. update as many rows as are expanded following that characterization.
	 * @param data Root entry of the tree.
	 * @param cargo Cargo type of the entry to be shown.
	 * @param source Source station of the entry to be shown.
	 * @param next Next station the cargo to be shown will visit.
	 * @param dest Final destination of the cargo to be shown.
	 * @param count Amount of cargo to be shown.
	 */
	void ShowCargo(CargoDataEntry *data, CargoType cargo, StationID source, StationID next, StationID dest, uint count)
	{
		if (count == 0) return;
		bool auto_distributed = _settings_game.linkgraph.GetDistributionType(cargo) != DT_MANUAL;
		const CargoDataEntry *expand = &this->expanded_rows;
		for (int i = 0; i < NUM_COLUMNS && expand != nullptr; ++i) {
			switch (groupings[i]) {
				case GR_CARGO:
					assert(i == 0);
					data = &data->InsertOrRetrieve(cargo);
					data->SetTransfers(source != this->window_number);
					expand = expand->Retrieve(cargo);
					break;
				case GR_SOURCE:
					if (auto_distributed || source != this->window_number) {
						data = &data->InsertOrRetrieve(source);
						expand = expand->Retrieve(source);
					}
					break;
				case GR_NEXT:
					if (auto_distributed) {
						data = &data->InsertOrRetrieve(next);
						expand = expand->Retrieve(next);
					}
					break;
				case GR_DESTINATION:
					if (auto_distributed) {
						data = &data->InsertOrRetrieve(dest);
						expand = expand->Retrieve(dest);
					}
					break;
			}
		}
		data->Update(count);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_SV_WAITING:
				fill.height = resize.height = GetCharacterHeight(FS_NORMAL);
				size.height = 4 * resize.height + padding.height;
				this->expand_shrink_width = std::max(GetStringBoundingBox("-").width, GetStringBoundingBox("+").width);
				break;

			case WID_SV_ACCEPT_RATING_LIST:
				size.height = ((this->GetWidget<NWidgetCore>(WID_SV_ACCEPTS_RATINGS)->GetString() == STR_STATION_VIEW_RATINGS_BUTTON) ? this->accepts_lines : this->rating_lines) * GetCharacterHeight(FS_NORMAL) + padding.height;
				break;

			case WID_SV_CLOSE_AIRPORT:
				if (!Station::Get(this->window_number)->facilities.Test(StationFacility::Airport)) {
					/* Hide 'Close Airport' button if no airport present. */
					size.width = 0;
					resize.width = 0;
					fill.width = 0;
				}
				break;
		}
	}

	void OnPaint() override
	{
		const Station *st = Station::Get(this->window_number);
		CargoDataEntry cargo;
		BuildCargoList(&cargo, st);

		this->vscroll->SetCount(cargo.GetNumChildren()); // update scrollbar

		/* disable some buttons */
		this->SetWidgetDisabledState(WID_SV_RENAME,   st->owner != _local_company);
		this->SetWidgetDisabledState(WID_SV_TRAINS,   !st->facilities.Test(StationFacility::Train));
		this->SetWidgetDisabledState(WID_SV_ROADVEHS, !st->facilities.Test(StationFacility::TruckStop) && !st->facilities.Test(StationFacility::BusStop));
		this->SetWidgetDisabledState(WID_SV_SHIPS,    !st->facilities.Test(StationFacility::Dock));
		this->SetWidgetDisabledState(WID_SV_PLANES,   !st->facilities.Test(StationFacility::Airport));
		this->SetWidgetDisabledState(WID_SV_CLOSE_AIRPORT, !st->facilities.Test(StationFacility::Airport) || st->owner != _local_company || st->owner == OWNER_NONE); // Also consider SE, where _local_company == OWNER_NONE
		this->SetWidgetLoweredState(WID_SV_CLOSE_AIRPORT, st->facilities.Test(StationFacility::Airport) && st->airport.blocks.Test(AirportBlock::AirportClosed));

		extern const Station *_viewport_highlight_station;
		this->SetWidgetDisabledState(WID_SV_CATCHMENT, st->facilities.None());
		this->SetWidgetLoweredState(WID_SV_CATCHMENT, _viewport_highlight_station == st);

		this->DrawWidgets();

		if (!this->IsShaded()) {
			/* Draw 'accepted cargo' or 'cargo ratings'. */
			const NWidgetBase *wid = this->GetWidget<NWidgetBase>(WID_SV_ACCEPT_RATING_LIST);
			const Rect r = wid->GetCurrentRect();
			if (this->GetWidget<NWidgetCore>(WID_SV_ACCEPTS_RATINGS)->GetString() == STR_STATION_VIEW_RATINGS_BUTTON) {
				int lines = this->DrawAcceptedCargo(r);
				if (lines > this->accepts_lines) { // Resize the widget, and perform re-initialization of the window.
					this->accepts_lines = lines;
					this->ReInit();
					return;
				}
			} else {
				int lines = this->DrawCargoRatings(r);
				if (lines > this->rating_lines) { // Resize the widget, and perform re-initialization of the window.
					this->rating_lines = lines;
					this->ReInit();
					return;
				}
			}

			/* Draw arrow pointing up/down for ascending/descending sorting */
			this->DrawSortButtonState(WID_SV_SORT_ORDER, sort_orders[1] == SO_ASCENDING ? SBS_UP : SBS_DOWN);

			int pos = this->vscroll->GetPosition();

			int maxrows = this->vscroll->GetCapacity();

			displayed_rows.clear();

			/* Draw waiting cargo. */
			NWidgetBase *nwi = this->GetWidget<NWidgetBase>(WID_SV_WAITING);
			Rect waiting_rect = nwi->GetCurrentRect().Shrink(WidgetDimensions::scaled.framerect);
			this->DrawEntries(cargo, waiting_rect, pos, maxrows, 0);
			scroll_to_row = INT_MAX;
		}
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget == WID_SV_CAPTION) {
			const Station *st = Station::Get(this->window_number);
			return GetString(STR_STATION_VIEW_CAPTION, st->index, st->facilities);
		}

		return this->Window::GetWidgetString(widget, stringid);
	}

	/**
	 * Rebuild the cache for estimated destinations which is used to quickly show the "destination" entries
	 * even if we actually don't know the destination of a certain packet from just looking at it.
	 * @param i Cargo to recalculate the cache for.
	 */
	void RecalcDestinations(CargoType cargo)
	{
		const Station *st = Station::Get(this->window_number);
		CargoDataEntry &entry = cached_destinations.InsertOrRetrieve(cargo);
		entry.Clear();

		if (!st->goods[cargo].HasData()) return;

		for (const auto &it : st->goods[cargo].GetData().flows) {
			StationID from = it.first;
			CargoDataEntry &source_entry = entry.InsertOrRetrieve(from);
			uint32_t prev_count = 0;
			for (const auto &flow_it : *it.second.GetShares()) {
				StationID via = flow_it.second;
				CargoDataEntry &via_entry = source_entry.InsertOrRetrieve(via);
				if (via == this->window_number) {
					via_entry.InsertOrRetrieve(via).Update(flow_it.first - prev_count);
				} else {
					EstimateDestinations(cargo, from, via, flow_it.first - prev_count, via_entry);
				}
				prev_count = flow_it.first;
			}
		}
	}

	/**
	 * Estimate the amounts of cargo per final destination for a given cargo, source station and next hop and
	 * save the result as children of the given CargoDataEntry.
	 * @param cargo type of the cargo to estimate destinations for.
	 * @param source Source station of the given batch of cargo.
	 * @param next Intermediate hop to start the calculation at ("next hop").
	 * @param count Size of the batch of cargo.
	 * @param dest CargoDataEntry to save the results in.
	 */
	void EstimateDestinations(CargoType cargo, StationID source, StationID next, uint count, CargoDataEntry &dest)
	{
		if (Station::IsValidID(next) && Station::IsValidID(source)) {
			GoodsEntry &ge = Station::Get(next)->goods[cargo];
			if (!ge.HasData()) return;

			CargoDataEntry tmp;
			const FlowStatMap &flowmap = ge.GetData().flows;
			FlowStatMap::const_iterator map_it = flowmap.find(source);
			if (map_it != flowmap.end()) {
				const FlowStat::SharesMap *shares = map_it->second.GetShares();
				uint32_t prev_count = 0;
				for (FlowStat::SharesMap::const_iterator i = shares->begin(); i != shares->end(); ++i) {
					tmp.InsertOrRetrieve(i->second).Update(i->first - prev_count);
					prev_count = i->first;
				}
			}

			if (tmp.GetCount() == 0) {
				dest.InsertOrRetrieve(StationID::Invalid()).Update(count);
			} else {
				uint sum_estimated = 0;
				while (sum_estimated < count) {
					for (CargoDataSet::iterator i = tmp.Begin(); i != tmp.End() && sum_estimated < count; ++i) {
						CargoDataEntry &child = **i;
						uint estimate = DivideApprox(child.GetCount() * count, tmp.GetCount());
						if (estimate == 0) estimate = 1;

						sum_estimated += estimate;
						if (sum_estimated > count) {
							estimate -= sum_estimated - count;
							sum_estimated = count;
						}

						if (estimate > 0) {
							if (child.GetStation() == next) {
								dest.InsertOrRetrieve(next).Update(estimate);
							} else {
								EstimateDestinations(cargo, source, child.GetStation(), estimate, dest);
							}
						}
					}

				}
			}
		} else {
			dest.InsertOrRetrieve(StationID::Invalid()).Update(count);
		}
	}

	/**
	 * Build up the cargo view for PLANNED mode and a specific cargo.
	 * @param cargo Cargo to show.
	 * @param flows The current station's flows for that cargo.
	 * @param entry The CargoDataEntry to save the results in.
	 */
	void BuildFlowList(CargoType cargo, const FlowStatMap &flows, CargoDataEntry *entry)
	{
		const CargoDataEntry *source_dest = this->cached_destinations.Retrieve(cargo);
		for (FlowStatMap::const_iterator it = flows.begin(); it != flows.end(); ++it) {
			StationID from = it->first;
			const CargoDataEntry *source_entry = source_dest->Retrieve(from);
			const FlowStat::SharesMap *shares = it->second.GetShares();
			for (FlowStat::SharesMap::const_iterator flow_it = shares->begin(); flow_it != shares->end(); ++flow_it) {
				const CargoDataEntry *via_entry = source_entry->Retrieve(flow_it->second);
				for (CargoDataSet::iterator dest_it = via_entry->Begin(); dest_it != via_entry->End(); ++dest_it) {
					CargoDataEntry &dest_entry = **dest_it;
					ShowCargo(entry, cargo, from, flow_it->second, dest_entry.GetStation(), dest_entry.GetCount());
				}
			}
		}
	}

	/**
	 * Build up the cargo view for WAITING mode and a specific cargo.
	 * @param cargo Cargo to show.
	 * @param packets The current station's cargo list for that cargo.
	 * @param entry The CargoDataEntry to save the result in.
	 */
	void BuildCargoList(CargoType cargo, const StationCargoList &packets, CargoDataEntry *entry)
	{
		const CargoDataEntry *source_dest = this->cached_destinations.Retrieve(cargo);
		for (StationCargoList::ConstIterator it = packets.Packets()->begin(); it != packets.Packets()->end(); it++) {
			const CargoPacket *cp = *it;
			StationID next = it.GetKey();

			const CargoDataEntry *source_entry = source_dest->Retrieve(cp->GetFirstStation());
			if (source_entry == nullptr) {
				this->ShowCargo(entry, cargo, cp->GetFirstStation(), next, StationID::Invalid(), cp->Count());
				continue;
			}

			const CargoDataEntry *via_entry = source_entry->Retrieve(next);
			if (via_entry == nullptr) {
				this->ShowCargo(entry, cargo, cp->GetFirstStation(), next, StationID::Invalid(), cp->Count());
				continue;
			}

			uint remaining = cp->Count();
			for (CargoDataSet::iterator dest_it = via_entry->Begin(); dest_it != via_entry->End();) {
				CargoDataEntry &dest_entry = **dest_it;

				/* Advance iterator here instead of in the for statement to test whether this is the last entry */
				++dest_it;

				uint val;
				if (dest_it == via_entry->End()) {
					/* Allocate all remaining waiting cargo to the last destination to avoid
					 * waiting cargo being "lost", and the displayed total waiting cargo
					 * not matching GoodsEntry::TotalCount() */
					val = remaining;
				} else {
					val = std::min<uint>(remaining, DivideApprox(cp->Count() * dest_entry.GetCount(), via_entry->GetCount()));
					remaining -= val;
				}
				this->ShowCargo(entry, cargo, cp->GetFirstStation(), next, dest_entry.GetStation(), val);
			}
		}
		this->ShowCargo(entry, cargo, NEW_STATION, NEW_STATION, NEW_STATION, packets.ReservedCount());
	}

	/**
	 * Build up the cargo view for all cargoes.
	 * @param entry The root cargo entry to save all results in.
	 * @param st The station to calculate the cargo view from.
	 */
	void BuildCargoList(CargoDataEntry *entry, const Station *st)
	{
		for (CargoType cargo = 0; cargo < NUM_CARGO; ++cargo) {

			if (this->cached_destinations.Retrieve(cargo) == nullptr) {
				this->RecalcDestinations(cargo);
			}

			const GoodsEntry &ge = st->goods[cargo];
			if (!ge.HasData()) continue;

			if (this->current_mode == MODE_WAITING) {
				this->BuildCargoList(cargo, ge.GetData().cargo, entry);
			} else {
				this->BuildFlowList(cargo, ge.GetData().flows, entry);
			}
		}
	}

	/**
	 * Mark a specific row, characterized by its CargoDataEntry, as expanded.
	 * @param entry The row to be marked as expanded.
	 */
	void SetDisplayedRow(const CargoDataEntry &entry)
	{
		std::list<StationID> stations;
		const CargoDataEntry *parent = entry.GetParent();
		if (parent->GetParent() == nullptr) {
			this->displayed_rows.push_back(RowDisplay(&this->expanded_rows, entry.GetCargo()));
			return;
		}

		StationID next = entry.GetStation();
		while (parent->GetParent()->GetParent() != nullptr) {
			stations.push_back(parent->GetStation());
			parent = parent->GetParent();
		}

		CargoType cargo = parent->GetCargo();
		CargoDataEntry *filter = this->expanded_rows.Retrieve(cargo);
		while (!stations.empty()) {
			filter = filter->Retrieve(stations.back());
			stations.pop_back();
		}

		this->displayed_rows.push_back(RowDisplay(filter, next));
	}

	/**
	 * Select the correct string for an entry referring to the specified station.
	 * @param station Station the entry is showing cargo for.
	 * @param here String to be shown if the entry refers to the same station as this station GUI belongs to.
	 * @param other_station String to be shown if the entry refers to a specific other station.
	 * @param any String to be shown if the entry refers to "any station".
	 * @return One of the three given strings or STR_STATION_VIEW_RESERVED, depending on what station the entry refers to.
	 */
	StringID GetEntryString(StationID station, StringID here, StringID other_station, StringID any) const
	{
		if (station == this->window_number) {
			return here;
		} else if (station == StationID::Invalid()) {
			return any;
		} else if (station == NEW_STATION) {
			return STR_STATION_VIEW_RESERVED;
		} else {
			return other_station;
		}
	}

	StringID GetGroupingString(Grouping grouping, StationID station) const
	{
		switch (grouping) {
			case GR_SOURCE: return this->GetEntryString(station, STR_STATION_VIEW_FROM_HERE, STR_STATION_VIEW_FROM, STR_STATION_VIEW_FROM_ANY);
			case GR_NEXT: return this->GetEntryString(station, STR_STATION_VIEW_VIA_HERE, STR_STATION_VIEW_VIA, STR_STATION_VIEW_VIA_ANY);
			case GR_DESTINATION: return this->GetEntryString(station, STR_STATION_VIEW_TO_HERE, STR_STATION_VIEW_TO, STR_STATION_VIEW_TO_ANY);
			default: NOT_REACHED();
		}
	}

	/**
	 * Determine if we need to show the special "non-stop" string.
	 * @param cd Entry we are going to show.
	 * @param station Station the entry refers to.
	 * @param column The "column" the entry will be shown in.
	 * @return either STR_STATION_VIEW_VIA or STR_STATION_VIEW_NONSTOP.
	 */
	StringID SearchNonStop(CargoDataEntry &cd, StationID station, int column)
	{
		assert(column < NUM_COLUMNS);
		CargoDataEntry *parent = cd.GetParent();
		for (int i = column - 1; i > 0; --i) {
			if (this->groupings[i] == GR_DESTINATION) {
				if (parent->GetStation() == station) {
					return STR_STATION_VIEW_NONSTOP;
				} else {
					return STR_STATION_VIEW_VIA;
				}
			}
			parent = parent->GetParent();
		}

		if (column < NUM_COLUMNS - 1 && this->groupings[column + 1] == GR_DESTINATION) {
			CargoDataSet::iterator begin = cd.Begin();
			CargoDataSet::iterator end = cd.End();
			if (begin != end && ++(cd.Begin()) == end && (*(begin))->GetStation() == station) {
				return STR_STATION_VIEW_NONSTOP;
			} else {
				return STR_STATION_VIEW_VIA;
			}
		}

		return STR_STATION_VIEW_VIA;
	}

	/**
	 * Draw the given cargo entries in the station GUI.
	 * @param entry Root entry for all cargo to be drawn.
	 * @param r Screen rectangle to draw into.
	 * @param pos Current row to be drawn to (counted down from 0 to -maxrows, same as vscroll->GetPosition()).
	 * @param maxrows Maximum row to be drawn.
	 * @param column Current "column" being drawn.
	 * @param cargo Current cargo being drawn (if cargo column has been passed).
	 * @return row (in "pos" counting) after the one we have last drawn to.
	 */
	int DrawEntries(CargoDataEntry &entry, const Rect &r, int pos, int maxrows, int column, CargoType cargo = INVALID_CARGO)
	{
		assert(column < NUM_COLUMNS);
		if (this->sortings[column] == CargoSortType::AsGrouping) {
			if (this->groupings[column] != GR_CARGO) {
				entry.Resort(CargoSortType::StationString, this->sort_orders[column]);
			}
		} else {
			entry.Resort(CargoSortType::Count, this->sort_orders[column]);
		}
		for (CargoDataSet::iterator i = entry.Begin(); i != entry.End(); ++i) {
			CargoDataEntry &cd = **i;

			Grouping grouping = this->groupings[column];
			if (grouping == GR_CARGO) cargo = cd.GetCargo();
			bool auto_distributed = _settings_game.linkgraph.GetDistributionType(cargo) != DT_MANUAL;

			if (pos > -maxrows && pos <= 0) {
				StringID str = STR_EMPTY;
				StationID station = StationID::Invalid();
				int y = r.top - pos * GetCharacterHeight(FS_NORMAL);
				if (this->groupings[column] == GR_CARGO) {
					str = STR_STATION_VIEW_WAITING_CARGO;
					DrawCargoIcons(cd.GetCargo(), cd.GetCount(), r.left + this->expand_shrink_width, r.right - this->expand_shrink_width, y);
				} else {
					if (!auto_distributed) grouping = GR_SOURCE;
					station = cd.GetStation();
					str = this->GetGroupingString(grouping, station);
					if (grouping == GR_NEXT && str == STR_STATION_VIEW_VIA) str = this->SearchNonStop(cd, station, column);

					if (pos == -this->scroll_to_row && Station::IsValidID(station)) {
						ScrollMainWindowToTile(Station::Get(station)->xy);
					}
				}

				bool rtl = _current_text_dir == TD_RTL;
				Rect text = r.Indent(column * WidgetDimensions::scaled.hsep_indent, rtl).Indent(this->expand_shrink_width, !rtl);
				Rect shrink = r.WithWidth(this->expand_shrink_width, !rtl);

				DrawString(text.left, text.right, y, GetString(str, cargo, cd.GetCount(), station));

				if (column < NUM_COLUMNS - 1) {
					std::string_view sym;
					if (cd.GetNumChildren() > 0) {
						sym = "-";
					} else if (auto_distributed && str != STR_STATION_VIEW_RESERVED) {
						sym = "+";
					} else {
						/* Only draw '+' if there is something to be shown. */
						const GoodsEntry &ge = Station::Get(this->window_number)->goods[cargo];
						if (ge.HasData()) {
							const StationCargoList &cargo_list = ge.GetData().cargo;
							if (grouping == GR_CARGO && (cargo_list.ReservedCount() > 0 || cd.HasTransfers())) {
								sym = "+";
							}
						}
					}
					if (!sym.empty()) DrawString(shrink.left, shrink.right, y, sym, TC_YELLOW);
				}
				this->SetDisplayedRow(cd);
			}
			--pos;
			if ((auto_distributed || column == 0) && column < NUM_COLUMNS - 1) {
				pos = this->DrawEntries(cd, r, pos, maxrows, column + 1, cargo);
			}
		}
		return pos;
	}

	/**
	 * Draw accepted cargo in the #WID_SV_ACCEPT_RATING_LIST widget.
	 * @param r Rectangle of the widget.
	 * @return Number of lines needed for drawing the accepted cargo.
	 */
	int DrawAcceptedCargo(const Rect &r) const
	{
		const Station *st = Station::Get(this->window_number);
		Rect tr = r.Shrink(WidgetDimensions::scaled.framerect);

		int bottom = DrawStringMultiLine(tr.left, tr.right, tr.top, INT32_MAX, GetString(STR_STATION_VIEW_ACCEPTS_CARGO, GetAcceptanceMask(st)));
		return CeilDiv(bottom - r.top - WidgetDimensions::scaled.framerect.top, GetCharacterHeight(FS_NORMAL));
	}

	/**
	 * Draw cargo ratings in the #WID_SV_ACCEPT_RATING_LIST widget.
	 * @param r Rectangle of the widget.
	 * @return Number of lines needed for drawing the cargo ratings.
	 */
	int DrawCargoRatings(const Rect &r) const
	{
		const Station *st = Station::Get(this->window_number);
		bool rtl = _current_text_dir == TD_RTL;
		Rect tr = r.Shrink(WidgetDimensions::scaled.framerect);

		if (st->town->exclusive_counter > 0) {
			tr.top = DrawStringMultiLine(tr, GetString(st->town->exclusivity == st->owner ? STR_STATION_VIEW_EXCLUSIVE_RIGHTS_SELF : STR_STATION_VIEW_EXCLUSIVE_RIGHTS_COMPANY, st->town->exclusivity));
			tr.top += WidgetDimensions::scaled.vsep_wide;
		}

		DrawString(tr, TimerGameEconomy::UsingWallclockUnits() ? STR_STATION_VIEW_SUPPLY_RATINGS_TITLE_MINUTE : STR_STATION_VIEW_SUPPLY_RATINGS_TITLE_MONTH);
		tr.top += GetCharacterHeight(FS_NORMAL);

		for (const CargoSpec *cs : _sorted_standard_cargo_specs) {
			const GoodsEntry *ge = &st->goods[cs->Index()];
			if (!ge->HasRating()) continue;

			const LinkGraph *lg = LinkGraph::GetIfValid(ge->link_graph);
			DrawString(tr.Indent(WidgetDimensions::scaled.hsep_indent, rtl),
				GetString(STR_STATION_VIEW_CARGO_SUPPLY_RATING,
					cs->name,
					lg != nullptr ? lg->Monthly((*lg)[ge->node].supply) : 0,
					STR_CARGO_RATING_APPALLING + (ge->rating >> 5),
					ToPercent8(ge->rating)));
			tr.top += GetCharacterHeight(FS_NORMAL);
		}
		return CeilDiv(tr.top - r.top - WidgetDimensions::scaled.framerect.top, GetCharacterHeight(FS_NORMAL));
	}

	/**
	 * Expand or collapse a specific row.
	 * @param filter Parent of the row.
	 * @param next ID pointing to the row.
	 */
	template <class Tid>
	void HandleCargoWaitingClick(CargoDataEntry *filter, Tid next)
	{
		if (filter->Retrieve(next) != nullptr) {
			filter->Remove(next);
		} else {
			filter->InsertOrRetrieve(next);
		}
	}

	/**
	 * Handle a click on a specific row in the cargo view.
	 * @param row Row being clicked.
	 */
	void HandleCargoWaitingClick(int row)
	{
		if (row < 0 || (uint)row >= this->displayed_rows.size()) return;
		if (_ctrl_pressed) {
			this->scroll_to_row = row;
		} else {
			RowDisplay &display = this->displayed_rows[row];
			if (display.filter == &this->expanded_rows) {
				this->HandleCargoWaitingClick<CargoType>(display.filter, display.next_cargo);
			} else {
				this->HandleCargoWaitingClick<StationID>(display.filter, display.next_station);
			}
		}
		this->SetWidgetDirty(WID_SV_WAITING);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_SV_WAITING:
				this->HandleCargoWaitingClick(this->vscroll->GetScrolledRowFromWidget(pt.y, this, WID_SV_WAITING, WidgetDimensions::scaled.framerect.top) - this->vscroll->GetPosition());
				break;

			case WID_SV_CATCHMENT:
				SetViewportCatchmentStation(Station::Get(this->window_number), !this->IsWidgetLowered(WID_SV_CATCHMENT));
				break;

			case WID_SV_LOCATION:
				if (_ctrl_pressed) {
					ShowExtraViewportWindow(Station::Get(this->window_number)->xy);
				} else {
					ScrollMainWindowToTile(Station::Get(this->window_number)->xy);
				}
				break;

			case WID_SV_ACCEPTS_RATINGS: {
				/* Swap between 'accepts' and 'ratings' view. */
				int height_change;
				NWidgetCore *nwi = this->GetWidget<NWidgetCore>(WID_SV_ACCEPTS_RATINGS);
				if (this->GetWidget<NWidgetCore>(WID_SV_ACCEPTS_RATINGS)->GetString() == STR_STATION_VIEW_RATINGS_BUTTON) {
					nwi->SetStringTip(STR_STATION_VIEW_ACCEPTS_BUTTON, STR_STATION_VIEW_ACCEPTS_TOOLTIP); // Switch to accepts view.
					height_change = this->rating_lines - this->accepts_lines;
				} else {
					nwi->SetStringTip(STR_STATION_VIEW_RATINGS_BUTTON, STR_STATION_VIEW_RATINGS_TOOLTIP); // Switch to ratings view.
					height_change = this->accepts_lines - this->rating_lines;
				}
				this->ReInit(0, height_change * GetCharacterHeight(FS_NORMAL));
				break;
			}

			case WID_SV_RENAME:
				ShowQueryString(GetString(STR_STATION_NAME, this->window_number), STR_STATION_VIEW_RENAME_STATION_CAPTION, MAX_LENGTH_STATION_NAME_CHARS,
						this, CS_ALPHANUMERAL, {QueryStringFlag::EnableDefault, QueryStringFlag::LengthIsInChars});
				break;

			case WID_SV_CLOSE_AIRPORT:
				Command<CMD_OPEN_CLOSE_AIRPORT>::Post(this->window_number);
				break;

			case WID_SV_TRAINS:   // Show list of scheduled trains to this station
			case WID_SV_ROADVEHS: // Show list of scheduled road-vehicles to this station
			case WID_SV_SHIPS:    // Show list of scheduled ships to this station
			case WID_SV_PLANES: { // Show list of scheduled aircraft to this station
				Owner owner = Station::Get(this->window_number)->owner;
				ShowVehicleListWindow(owner, (VehicleType)(widget - WID_SV_TRAINS), static_cast<StationID>(this->window_number));
				break;
			}

			case WID_SV_SORT_BY: {
				/* The initial selection is composed of current mode and
				 * sorting criteria for columns 1, 2, and 3. Column 0 is always
				 * sorted by cargo type. The others can theoretically be sorted
				 * by different things but there is no UI for that. */
				ShowDropDownMenu(this, StationViewWindow::sort_names,
						this->current_mode * 2 + (this->sortings[1] == CargoSortType::Count ? 1 : 0),
						WID_SV_SORT_BY, 0, 0);
				break;
			}

			case WID_SV_GROUP_BY: {
				ShowDropDownMenu(this, StationViewWindow::group_names, this->grouping_index, WID_SV_GROUP_BY, 0, 0);
				break;
			}

			case WID_SV_SORT_ORDER: { // flip sorting method asc/desc
				this->SelectSortOrder(this->sort_orders[1] == SO_ASCENDING ? SO_DESCENDING : SO_ASCENDING);
				this->SetTimeout();
				this->LowerWidget(WID_SV_SORT_ORDER);
				break;
			}
		}
	}

	/**
	 * Select a new sort order for the cargo view.
	 * @param order New sort order.
	 */
	void SelectSortOrder(SortOrder order)
	{
		this->sort_orders[1] = this->sort_orders[2] = this->sort_orders[3] = order;
		_settings_client.gui.station_gui_sort_order = this->sort_orders[1];
		this->SetDirty();
	}

	/**
	 * Select a new sort criterium for the cargo view.
	 * @param index Row being selected in the sort criteria drop down.
	 */
	void SelectSortBy(int index)
	{
		_settings_client.gui.station_gui_sort_by = index;
		switch (StationViewWindow::sort_names[index]) {
			case STR_STATION_VIEW_WAITING_STATION:
				this->current_mode = MODE_WAITING;
				this->sortings[1] = this->sortings[2] = this->sortings[3] = CargoSortType::AsGrouping;
				break;
			case STR_STATION_VIEW_WAITING_AMOUNT:
				this->current_mode = MODE_WAITING;
				this->sortings[1] = this->sortings[2] = this->sortings[3] = CargoSortType::Count;
				break;
			case STR_STATION_VIEW_PLANNED_STATION:
				this->current_mode = MODE_PLANNED;
				this->sortings[1] = this->sortings[2] = this->sortings[3] = CargoSortType::AsGrouping;
				break;
			case STR_STATION_VIEW_PLANNED_AMOUNT:
				this->current_mode = MODE_PLANNED;
				this->sortings[1] = this->sortings[2] = this->sortings[3] = CargoSortType::Count;
				break;
			default:
				NOT_REACHED();
		}
		/* Display the current sort variant */
		this->GetWidget<NWidgetCore>(WID_SV_SORT_BY)->SetString(StationViewWindow::sort_names[index]);
		this->SetDirty();
	}

	/**
	 * Select a new grouping mode for the cargo view.
	 * @param index Row being selected in the grouping drop down.
	 */
	void SelectGroupBy(int index)
	{
		this->grouping_index = index;
		_settings_client.gui.station_gui_group_order = index;
		this->GetWidget<NWidgetCore>(WID_SV_GROUP_BY)->SetString(StationViewWindow::group_names[index]);
		switch (StationViewWindow::group_names[index]) {
			case STR_STATION_VIEW_GROUP_S_V_D:
				this->groupings[1] = GR_SOURCE;
				this->groupings[2] = GR_NEXT;
				this->groupings[3] = GR_DESTINATION;
				break;
			case STR_STATION_VIEW_GROUP_S_D_V:
				this->groupings[1] = GR_SOURCE;
				this->groupings[2] = GR_DESTINATION;
				this->groupings[3] = GR_NEXT;
				break;
			case STR_STATION_VIEW_GROUP_V_S_D:
				this->groupings[1] = GR_NEXT;
				this->groupings[2] = GR_SOURCE;
				this->groupings[3] = GR_DESTINATION;
				break;
			case STR_STATION_VIEW_GROUP_V_D_S:
				this->groupings[1] = GR_NEXT;
				this->groupings[2] = GR_DESTINATION;
				this->groupings[3] = GR_SOURCE;
				break;
			case STR_STATION_VIEW_GROUP_D_S_V:
				this->groupings[1] = GR_DESTINATION;
				this->groupings[2] = GR_SOURCE;
				this->groupings[3] = GR_NEXT;
				break;
			case STR_STATION_VIEW_GROUP_D_V_S:
				this->groupings[1] = GR_DESTINATION;
				this->groupings[2] = GR_NEXT;
				this->groupings[3] = GR_SOURCE;
				break;
		}
		this->SetDirty();
	}

	void OnDropdownSelect(WidgetID widget, int index, int) override
	{
		if (widget == WID_SV_SORT_BY) {
			this->SelectSortBy(index);
		} else {
			this->SelectGroupBy(index);
		}
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		if (!str.has_value()) return;

		Command<CMD_RENAME_STATION>::Post(STR_ERROR_CAN_T_RENAME_STATION, this->window_number, *str);
	}

	void OnResize() override
	{
		this->vscroll->SetCapacityFromWidget(this, WID_SV_WAITING, WidgetDimensions::scaled.framerect.Vertical());
	}

	/**
	 * Some data on this window has become invalid. Invalidate the cache for the given cargo if necessary.
	 * @param data Information about the changed data. If it's a valid cargo type, invalidate the cargo data.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (gui_scope) {
			if (data >= 0 && data < NUM_CARGO) {
				this->cached_destinations.Remove((CargoType)data);
			} else {
				this->ReInit();
			}
		}
	}
};

static WindowDesc _station_view_desc(
	WDP_AUTO, "view_station", 249, 117,
	WC_STATION_VIEW, WC_NONE,
	{},
	_nested_station_view_widgets
);

/**
 * Opens StationViewWindow for given station
 *
 * @param station station which window should be opened
 */
void ShowStationViewWindow(StationID station)
{
	AllocateWindowDescFront<StationViewWindow>(_station_view_desc, station);
}

/** Struct containing TileIndex and StationID */
struct TileAndStation {
	TileIndex tile;    ///< TileIndex
	StationID station; ///< StationID
};

static std::vector<TileAndStation> _deleted_stations_nearby;
static std::vector<StationID> _stations_nearby_list;

/**
 * Add station on this tile to _stations_nearby_list if it's fully within the
 * station spread.
 * @param tile Tile just being checked
 * @param ctx Pointer to TileArea context
 * @tparam T the station filter type
 */
template <class T>
static void AddNearbyStation(TileIndex tile, TileArea *ctx)
{
	/* First check if there were deleted stations here */
	for (auto it = _deleted_stations_nearby.begin(); it != _deleted_stations_nearby.end(); /* nothing */) {
		if (it->tile == tile) {
			_stations_nearby_list.push_back(it->station);
			it = _deleted_stations_nearby.erase(it);
		} else {
			++it;
		}
	}

	/* Check if own station and if we stay within station spread */
	if (!IsTileType(tile, MP_STATION)) return;

	StationID sid = GetStationIndex(tile);

	/* This station is (likely) a waypoint */
	if (!T::IsValidID(sid)) return;

	BaseStation *st = BaseStation::Get(sid);
	if (st->owner != _local_company || std::ranges::find(_stations_nearby_list, sid) != _stations_nearby_list.end()) return;

	if (st->rect.BeforeAddRect(ctx->tile, ctx->w, ctx->h, StationRect::ADD_TEST).Succeeded()) {
		_stations_nearby_list.push_back(sid);
	}
}

/**
 * Circulate around the to-be-built station to find stations we could join.
 * Make sure that only stations are returned where joining wouldn't exceed
 * station spread and are our own station.
 * @param ta Base tile area of the to-be-built station
 * @param distant_join Search for adjacent stations (false) or stations fully
 *                     within station spread
 * @tparam T the station filter type, for stations to look for
 */
template <class T>
static const BaseStation *FindStationsNearby(TileArea ta, bool distant_join)
{
	TileArea ctx = ta;

	_stations_nearby_list.clear();
	_stations_nearby_list.push_back(NEW_STATION);
	_deleted_stations_nearby.clear();

	/* Check the inside, to return, if we sit on another station */
	for (TileIndex t : ta) {
		if (t < Map::Size() && IsTileType(t, MP_STATION) && T::IsValidID(GetStationIndex(t))) return BaseStation::GetByTile(t);
	}

	/* Look for deleted stations */
	for (const BaseStation *st : BaseStation::Iterate()) {
		if (T::IsValidBaseStation(st) && !st->IsInUse() && st->owner == _local_company) {
			/* Include only within station spread (yes, it is strictly less than) */
			if (std::max(DistanceMax(ta.tile, st->xy), DistanceMax(TileAddXY(ta.tile, ta.w - 1, ta.h - 1), st->xy)) < _settings_game.station.station_spread) {
				_deleted_stations_nearby.emplace_back(st->xy, st->index);

				/* Add the station when it's within where we're going to build */
				if (IsInsideBS(TileX(st->xy), TileX(ctx.tile), ctx.w) &&
						IsInsideBS(TileY(st->xy), TileY(ctx.tile), ctx.h)) {
					AddNearbyStation<T>(st->xy, &ctx);
				}
			}
		}
	}

	/* Only search tiles where we have a chance to stay within the station spread.
	 * The complete check needs to be done in the callback as we don't know the
	 * extent of the found station, yet. */
	if (distant_join && std::min(ta.w, ta.h) >= _settings_game.station.station_spread) return nullptr;
	uint max_dist = distant_join ? _settings_game.station.station_spread - std::min(ta.w, ta.h) : 1;

	for (auto tile : SpiralTileSequence(TileAddByDir(ctx.tile, DIR_N), max_dist, ta.w, ta.h)) {
		AddNearbyStation<T>(tile, &ctx);
	}

	return nullptr;
}

static constexpr NWidgetPart _nested_select_station_widgets[] = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, COLOUR_DARK_GREEN),
		NWidget(WWT_CAPTION, COLOUR_DARK_GREEN, WID_JS_CAPTION), SetStringTip(STR_JOIN_STATION_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_DEFSIZEBOX, COLOUR_DARK_GREEN),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, COLOUR_DARK_GREEN, WID_JS_PANEL), SetResize(1, 0), SetScrollbar(WID_JS_SCROLLBAR), EndContainer(),
		NWidget(NWID_VERTICAL),
			NWidget(NWID_VSCROLLBAR, COLOUR_DARK_GREEN, WID_JS_SCROLLBAR),
			NWidget(WWT_RESIZEBOX, COLOUR_DARK_GREEN),
		EndContainer(),
	EndContainer(),
};

/**
 * Window for selecting stations/waypoints to (distant) join to.
 * @tparam T The station filter type, for stations to join with
 */
template <class T>
struct SelectStationWindow : Window {
	StationPickerCmdProc select_station_proc{};
	TileArea area{}; ///< Location of new station
	Scrollbar *vscroll = nullptr;

	SelectStationWindow(WindowDesc &desc, TileArea ta, StationPickerCmdProc&& proc) :
		Window(desc),
		select_station_proc(std::move(proc)),
		area(ta)
	{
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_JS_SCROLLBAR);
		this->GetWidget<NWidgetCore>(WID_JS_CAPTION)->SetString(T::IsWaypoint() ? STR_JOIN_WAYPOINT_CAPTION : STR_JOIN_STATION_CAPTION);
		this->FinishInitNested(0);
		this->OnInvalidateData(0);

		_thd.freeze = true;
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		SetViewportCatchmentSpecializedStation<typename T::StationType>(nullptr, true);

		_thd.freeze = false;
		this->Window::Close();
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_JS_PANEL) return;

		/* Determine the widest string */
		Dimension d = GetStringBoundingBox(T::IsWaypoint() ? STR_JOIN_WAYPOINT_CREATE_SPLITTED_WAYPOINT : STR_JOIN_STATION_CREATE_SPLITTED_STATION);
		for (const auto &station : _stations_nearby_list) {
			if (station == NEW_STATION) continue;
			const BaseStation *st = BaseStation::Get(station);
			d = maxdim(d, GetStringBoundingBox(T::IsWaypoint()
				? GetString(STR_STATION_LIST_WAYPOINT, st->index)
				: GetString(STR_STATION_LIST_STATION, st->index, st->facilities)));
		}

		fill.height = resize.height = d.height;
		d.height *= 5;
		d.width += padding.width;
		d.height += padding.height;
		size = d;
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_JS_PANEL) return;

		Rect tr = r.Shrink(WidgetDimensions::scaled.framerect);
		auto [first, last] = this->vscroll->GetVisibleRangeIterators(_stations_nearby_list);
		for (auto it = first; it != last; ++it, tr.top += this->resize.step_height) {
			if (*it == NEW_STATION) {
				DrawString(tr, T::IsWaypoint() ? STR_JOIN_WAYPOINT_CREATE_SPLITTED_WAYPOINT : STR_JOIN_STATION_CREATE_SPLITTED_STATION);
			} else {
				const BaseStation *st = BaseStation::Get(*it);
				DrawString(tr, T::IsWaypoint()
					? GetString(STR_STATION_LIST_WAYPOINT, st->index)
					: GetString(STR_STATION_LIST_STATION, st->index, st->facilities));
			}
		}

	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget != WID_JS_PANEL) return;

		auto it = this->vscroll->GetScrolledItemFromWidget(_stations_nearby_list, pt.y, this, WID_JS_PANEL, WidgetDimensions::scaled.framerect.top);
		if (it == _stations_nearby_list.end()) return;

		/* Execute stored Command */
		this->select_station_proc(false, *it);

		/* Close Window; this might cause double frees! */
		CloseWindowById(WC_SELECT_STATION, 0);
	}

	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
	{
		if (_thd.dirty & 2) {
			_thd.dirty &= ~2;
			this->SetDirty();
		}
	}

	void OnResize() override
	{
		this->vscroll->SetCapacityFromWidget(this, WID_JS_PANEL, WidgetDimensions::scaled.framerect.Vertical());
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data Information about the changed data.
	 * @param gui_scope Whether the call is done from GUI scope. You may not do everything when not in GUI scope. See #InvalidateWindowData() for details.
	 */
	void OnInvalidateData([[maybe_unused]] int data = 0, [[maybe_unused]] bool gui_scope = true) override
	{
		if (!gui_scope) return;
		FindStationsNearby<T>(this->area, true);
		this->vscroll->SetCount(_stations_nearby_list.size());
		this->SetDirty();
	}

	void OnMouseOver([[maybe_unused]] Point pt, WidgetID widget) override
	{
		if (widget != WID_JS_PANEL) {
			SetViewportCatchmentSpecializedStation<typename T::StationType>(nullptr, true);
			return;
		}

		/* Show coverage area of station under cursor */
		auto it = this->vscroll->GetScrolledItemFromWidget(_stations_nearby_list, pt.y, this, WID_JS_PANEL, WidgetDimensions::scaled.framerect.top);
		const typename T::StationType *st = it == _stations_nearby_list.end() || *it == NEW_STATION ? nullptr : T::StationType::Get(*it);
		SetViewportCatchmentSpecializedStation<typename T::StationType>(st, true);
	}
};

static WindowDesc _select_station_desc(
	WDP_AUTO, "build_station_join", 200, 180,
	WC_SELECT_STATION, WC_NONE,
	WindowDefaultFlag::Construction,
	_nested_select_station_widgets
);


/**
 * Check whether we need to show the station selection window.
 * @param cmd Command to build the station.
 * @param ta Tile area of the to-be-built station
 * @tparam T the station filter type
 * @return whether we need to show the station selection window.
 */
template <class T>
static bool StationJoinerNeeded(TileArea ta, const StationPickerCmdProc &proc)
{
	/* Only show selection if distant join is enabled in the settings */
	if (!_settings_game.station.distant_join_stations) return false;

	/* If a window is already opened and we didn't ctrl-click,
	 * return true (i.e. just flash the old window) */
	Window *selection_window = FindWindowById(WC_SELECT_STATION, 0);
	if (selection_window != nullptr) {
		/* Abort current distant-join and start new one */
		selection_window->Close();
		UpdateTileSelection();
	}

	/* only show the popup, if we press ctrl */
	if (!_ctrl_pressed) return false;

	/* Now check if we could build there */
	if (!proc(true, StationID::Invalid())) return false;

	return FindStationsNearby<T>(ta, false) == nullptr;
}

/**
 * Show the station selection window when needed. If not, build the station.
 * @param cmd Command to build the station.
 * @param ta Area to build the station in
 * @tparam the class to find stations for
 */
template <class T>
void ShowSelectBaseStationIfNeeded(TileArea ta, StationPickerCmdProc&& proc)
{
	if (StationJoinerNeeded<T>(ta, proc)) {
		if (!_settings_client.gui.persistent_buildingtools) ResetObjectToPlace();
		new SelectStationWindow<T>(_select_station_desc, ta, std::move(proc));
	} else {
		proc(false, StationID::Invalid());
	}
}

/**
 * Show the station selection window when needed. If not, build the station.
 * @param ta Area to build the station in
 * @param proc Function called to execute the build command.
 */
void ShowSelectStationIfNeeded(TileArea ta, StationPickerCmdProc proc)
{
	ShowSelectBaseStationIfNeeded<StationTypeFilter>(ta, std::move(proc));
}

/**
 * Show the rail waypoint selection window when needed. If not, build the waypoint.
 * @param ta Area to build the waypoint in
 * @param proc Function called to execute the build command.
 */
void ShowSelectRailWaypointIfNeeded(TileArea ta, StationPickerCmdProc proc)
{
	ShowSelectBaseStationIfNeeded<RailWaypointTypeFilter>(ta, std::move(proc));
}

/**
 * Show the road waypoint selection window when needed. If not, build the waypoint.
 * @param ta Area to build the waypoint in
 * @param proc Function called to execute the build command.
 */
void ShowSelectRoadWaypointIfNeeded(TileArea ta, StationPickerCmdProc proc)
{
	ShowSelectBaseStationIfNeeded<RoadWaypointTypeFilter>(ta, std::move(proc));
}
