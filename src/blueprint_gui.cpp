/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file blueprint_gui.cpp GUI of the rail blueprints.
 *
 * The blueprint window lists the saved blueprints and has the tools to copy an area into a new blueprint
 * and to paste the selected blueprint, rotated and mirrored. While pasting, the transformed blueprint follows
 * the mouse: the tile selection is a rectangle of the size of the transformed blueprint with its north corner
 * at the tile under the cursor, and the viewport draws its contents from #GetBlueprintPreviewTile.
 *
 * The window changes the game only through #StartBlueprintPaste, which sends network commands.
 */

#include "stdafx.h"
#include "blueprint.h"
#include "blueprint_gui.h"
#include "company_base.h"
#include "company_func.h"
#include "error.h"
#include "gfx_func.h"
#include "palette_func.h"
#include "gui.h"
#include "hotkeys.h"
#include "map_func.h"
#include "rail.h"
#include "rail_gui.h"
#include "strings_func.h"
#include "textbuf_gui.h"
#include "tilehighlight_func.h"
#include "track_func.h"
#include "viewport_func.h"
#include "window_func.h"
#include "window_gui.h"
#include "core/geometry_func.hpp"

#include "widgets/blueprint_widget.h"

#include "table/sprites.h"
#include "table/strings.h"

#include "safeguards.h"

/** Maximum length of a blueprint name, in characters. */
static constexpr uint MAX_LENGTH_BLUEPRINT_NAME_CHARS = 64;

/** Contents of the paste preview: the transformed blueprint as a grid of tiles. */
struct BlueprintPreview {
	bool active = false; ///< The blueprint window is placing a blueprint.
	uint width = 0; ///< Width of the transformed blueprint.
	uint height = 0; ///< Height of the transformed blueprint.
	std::vector<uint8_t> grid{}; ///< Per tile (row by row): track bits and #BlueprintPreviewFlag.

	/**
	 * Fill the grid from a blueprint.
	 * @param bp The blueprint, untransformed.
	 * @param transform The transformation to show.
	 */
	void Build(const Blueprint &bp, BlueprintTransform transform)
	{
		Blueprint tbp = bp.Transformed(transform);
		this->width = tbp.width;
		this->height = tbp.height;
		this->grid.assign(static_cast<size_t>(this->width) * this->height, 0);

		auto at = [this](uint x, uint y) -> uint8_t & { return this->grid[static_cast<size_t>(y) * this->width + x]; };
		const uint8_t mark = 1 << to_underlying(BlueprintPreviewFlag::Mark);

		for (const BlueprintTrack &t : tbp.tracks) at(t.x, t.y) |= t.tracks.base();
		for (const BlueprintDepot &d : tbp.depots) at(d.x, d.y) |= mark;
		for (const BlueprintStationTile &st : tbp.stations) at(st.x, st.y) |= mark | TrackBits{AxisToTrack(st.axis)}.base();
		for (const BlueprintTunnelBridge &tb : tbp.tunnelbridges) {
			at(tb.x, tb.y) |= mark;
			at(tb.end_x, tb.end_y) |= mark;
		}
	}
};

static BlueprintPreview _blueprint_preview; ///< The paste preview.

/**
 * Is a blueprint being placed, so the tile selection shows the paste preview?
 * @return True when the viewport should draw the preview inside the tile selection.
 */
bool IsBlueprintPreviewActive()
{
	return _blueprint_preview.active && _thd.window_class == WindowClass::Blueprints;
}

/**
 * Get what the paste preview shows on a tile.
 * @param x X offset from the north corner of the transformed blueprint.
 * @param y Y offset from the north corner of the transformed blueprint.
 * @return Track bits (#BLUEPRINT_PREVIEW_TRACK_MASK) and #BlueprintPreviewFlag bits; 0 outside the blueprint.
 */
uint8_t GetBlueprintPreviewTile(uint x, uint y)
{
	if (x >= _blueprint_preview.width || y >= _blueprint_preview.height) return 0;
	return _blueprint_preview.grid[static_cast<size_t>(y) * _blueprint_preview.width + x];
}

/** Paste with the rail type selected in the rail toolbar instead of the rail types of the blueprint. Remembered while the game runs. */
static bool _blueprint_use_selected_railtype = false;
/** Name of the blueprint selected last, to select it again when the window is opened. */
static std::string _blueprint_last_selected;

/** The window with the saved rail blueprints. */
struct BlueprintWindow : Window {
	/** A saved blueprint in the list. */
	struct Entry {
		std::string name{}; ///< Name of the blueprint.
		bool loaded = false; ///< The information below was read from the file.
		bool ok = false; ///< The file could be read.
		uint width = 0; ///< Width of the blueprint.
		uint height = 0; ///< Height of the blueprint.
		size_t items = 0; ///< Number of items.
	};

	/** What the window is doing with the mouse on the map. */
	enum class Mode : uint8_t {
		None, ///< Nothing.
		Copy, ///< Selecting an area to copy.
		Paste, ///< Placing the selected blueprint.
	};

	/** What the open query string is for. */
	enum class Query : uint8_t {
		None, ///< No query.
		NewName, ///< Name of a newly copied blueprint.
		Rename, ///< New name of the selected blueprint.
	};

	mutable std::vector<Entry> entries{}; ///< Saved blueprints; their information is read lazily when drawn.
	Scrollbar *vscroll = nullptr; ///< Scrollbar of the list.
	std::string selected{}; ///< Name of the selected blueprint, empty when none.
	std::optional<Blueprint> blueprint{}; ///< The selected blueprint.
	BlueprintTransform transform{}; ///< Rotation and mirror for pasting.
	Mode mode = Mode::None; ///< Current map tool.
	Query query = Query::None; ///< Purpose of the open query string.
	std::optional<Blueprint> captured{}; ///< Copied area waiting for its name.
	uint captured_partial = 0; ///< Bridges and tunnels left out of the copied area.

	BlueprintWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc)
	{
		this->CreateNestedTree();
		this->vscroll = this->GetScrollbar(WID_BP_SCROLLBAR);
		this->FinishInitNested(window_number);
		this->RebuildList();
		/* Select the blueprint selected last time, or the first one. */
		if (!_blueprint_last_selected.empty() && this->FindEntry(_blueprint_last_selected) != nullptr) {
			this->Select(std::string{_blueprint_last_selected});
		} else if (!this->entries.empty()) {
			this->Select(std::string{this->entries.front().name});
		}
		this->UpdateButtons();
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		_blueprint_preview.active = false;
		this->Window::Close();
	}

	/** Read the list of blueprint files again, keeping what is known about blueprints that are still there. */
	void RebuildList()
	{
		std::vector<Entry> old = std::move(this->entries);
		this->entries.clear();
		for (std::string &name : ListBlueprints()) {
			auto it = std::ranges::find(old, name, &Entry::name);
			if (it != old.end()) {
				this->entries.push_back(std::move(*it));
			} else {
				this->entries.push_back({std::move(name)});
			}
		}
		this->vscroll->SetCount(this->entries.size());

		if (!this->selected.empty() && this->FindEntry(this->selected) == nullptr) this->Select({});
		this->SetDirty();
	}

	/**
	 * Find a blueprint in the list.
	 * @param name Name of the blueprint.
	 * @return The entry, or nullptr when there is no such blueprint.
	 */
	Entry *FindEntry(std::string_view name) const
	{
		auto it = std::ranges::find(this->entries, name, &Entry::name);
		return it == this->entries.end() ? nullptr : &*it;
	}

	/**
	 * Forget the cached information of a blueprint, after its file was changed.
	 * @param name Name of the blueprint.
	 */
	void ForgetEntry(std::string_view name)
	{
		Entry *e = this->FindEntry(name);
		if (e != nullptr) e->loaded = false;
	}

	/**
	 * Read the information of a list entry, when not done yet.
	 * @param e The entry.
	 */
	void LoadEntry(Entry &e) const
	{
		if (e.loaded) return;
		e.loaded = true;
		std::optional<Blueprint> bp = LoadBlueprint(e.name);
		e.ok = bp.has_value();
		if (e.ok) {
			e.width = bp->width;
			e.height = bp->height;
			e.items = bp->CountItems();
		}
	}

	/**
	 * Select a blueprint and load it.
	 * @param name Name of the blueprint, or empty to select none.
	 */
	void Select(std::string_view name)
	{
		this->selected = name;
		_blueprint_last_selected = this->selected;
		this->blueprint.reset();
		if (!this->selected.empty()) {
			this->blueprint = LoadBlueprint(this->selected);
			Entry *e = this->FindEntry(this->selected);
			if (e != nullptr) {
				e->loaded = true;
				e->ok = this->blueprint.has_value();
				if (e->ok) {
					e->width = this->blueprint->width;
					e->height = this->blueprint->height;
					e->items = this->blueprint->CountItems();
				}
			}
			if (!this->blueprint.has_value()) {
				ShowErrorMessage(GetEncodedString(STR_ERROR_BLUEPRINT_CAN_T_LOAD), GetEncodedString(STR_ERROR_BLUEPRINT_FILE_BROKEN, this->selected), WarningLevel::Info);
			}
		}

		/* Scroll the selection into view. */
		auto it = std::ranges::find(this->entries, this->selected, &Entry::name);
		if (it != this->entries.end()) this->vscroll->ScrollTowards(static_cast<int>(it - this->entries.begin()));

		if (this->mode == Mode::Paste) {
			if (this->blueprint.has_value()) {
				this->UpdatePreview();
			} else {
				ResetObjectToPlace();
			}
		}
		this->UpdateButtons();
		this->SetDirty();
	}

	/** Enable and disable the buttons for the current state. */
	void UpdateButtons()
	{
		bool have = this->blueprint.has_value();
		this->SetWidgetDisabledState(WID_BP_PASTE, !have || !Company::IsValidID(_local_company));
		this->SetWidgetDisabledState(WID_BP_RENAME, this->selected.empty());
		this->SetWidgetDisabledState(WID_BP_DELETE, this->selected.empty());
		this->SetWidgetDisabledState(WID_BP_ABORT, !GetBlueprintPasteStatus().active);
		this->SetWidgetLoweredState(WID_BP_RAILTYPE, _blueprint_use_selected_railtype);
		this->SetWidgetLoweredState(WID_BP_COPY, this->mode == Mode::Copy);
		this->SetWidgetLoweredState(WID_BP_PASTE, this->mode == Mode::Paste);
		this->SetWidgetLoweredState(WID_BP_MIRROR, this->transform.mirror);
	}

	/** Size of the selected blueprint after the transformation. */
	std::pair<uint, uint> TransformedSize() const
	{
		if (!this->blueprint.has_value()) return {0, 0};
		return TransformSize(this->blueprint->width, this->blueprint->height, this->transform);
	}

	/** Rebuild the preview and the tile selection after the blueprint or the transformation changed. */
	void UpdatePreview()
	{
		if (this->mode != Mode::Paste || !this->blueprint.has_value()) return;
		_blueprint_preview.Build(*this->blueprint, this->transform);
		_blueprint_preview.active = true;
		auto [w, h] = this->TransformedSize();
		SetTileSelectSize(std::max(1U, w), std::max(1U, h));
		/* The contents change even when the size of the selection does not. */
		MarkWholeScreenDirty();
	}

	/**
	 * Change the transformation.
	 * @param transform The new transformation.
	 */
	void SetTransform(BlueprintTransform transform)
	{
		this->transform = transform;
		this->UpdatePreview();
		this->UpdateButtons();
		this->SetDirty();
	}

	/** Get a name for a new blueprint that is not in use yet. */
	std::string GetDefaultName() const
	{
		for (uint n = 1;; n++) {
			std::string name = GetString(STR_BLUEPRINT_DEFAULT_NAME, n);
			if (this->FindEntry(name) == nullptr) return name;
		}
	}

	/**
	 * Copy an area into a blueprint and ask for its name.
	 * @param start_tile One corner of the area.
	 * @param end_tile The opposite corner.
	 */
	void CaptureArea(TileIndex start_tile, TileIndex end_tile)
	{
		BlueprintCaptureStats stats;
		Blueprint bp = CaptureBlueprint(start_tile, end_tile, {}, &stats);
		if (bp.IsEmpty()) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_BLUEPRINT_CAN_T_COPY), GetEncodedString(STR_ERROR_BLUEPRINT_NO_RAIL), WarningLevel::Info, TileX(end_tile) * TILE_SIZE, TileY(end_tile) * TILE_SIZE);
			return;
		}
		this->captured = std::move(bp);
		this->captured_partial = stats.partial_tunnelbridges;
		this->query = Query::NewName;
		ShowQueryString(this->GetDefaultName(), STR_BLUEPRINT_QUERY_NAME_CAPTION, MAX_LENGTH_BLUEPRINT_NAME_CHARS, this, CS_ALPHANUMERAL, {QueryStringFlag::AcceptUnchanged, QueryStringFlag::LengthIsInChars});
	}

	/**
	 * Save the copied area under a name.
	 * @param name The name.
	 */
	void SaveCaptured(std::string_view name)
	{
		if (!this->captured.has_value()) return;
		if (this->FindEntry(name) != nullptr) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_BLUEPRINT_CAN_T_SAVE), GetEncodedString(STR_ERROR_BLUEPRINT_NAME_EXISTS), WarningLevel::Info);
			this->captured.reset();
			return;
		}
		this->captured->name = name;
		if (!SaveBlueprint(*this->captured)) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_BLUEPRINT_CAN_T_SAVE), GetEncodedString(STR_ERROR_BLUEPRINT_WRITE_FAILED), WarningLevel::Error);
			this->captured.reset();
			return;
		}
		std::string saved = this->captured->name;
		uint partial = this->captured_partial;
		this->captured.reset();

		this->ForgetEntry(saved);
		this->RebuildList();
		this->Select(saved);
		if (partial > 0) {
			ShowErrorMessage(GetEncodedString(STR_BLUEPRINT_SAVED, saved), GetEncodedString(STR_BLUEPRINT_PARTIAL_LEFT_OUT, partial), WarningLevel::Info);
		}
	}

	/**
	 * Rename the selected blueprint.
	 * @param name The new name.
	 */
	void RenameSelected(std::string_view name)
	{
		if (this->selected.empty() || name.empty() || name == this->selected) return;
		if (this->FindEntry(name) != nullptr) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_BLUEPRINT_CAN_T_RENAME), GetEncodedString(STR_ERROR_BLUEPRINT_NAME_EXISTS), WarningLevel::Info);
			return;
		}
		if (!RenameBlueprint(this->selected, name)) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_BLUEPRINT_CAN_T_RENAME), GetEncodedString(STR_ERROR_BLUEPRINT_WRITE_FAILED), WarningLevel::Error);
			return;
		}
		std::string new_name{name};
		this->RebuildList();
		this->Select(new_name);
	}

	/** Delete the selected blueprint (after confirmation). */
	void DeleteSelected()
	{
		if (this->selected.empty()) return;
		DeleteBlueprint(this->selected);
		this->RebuildList();
	}

	/**
	 * Paste the selected blueprint.
	 * @param tile North corner of the transformed blueprint.
	 */
	void Paste(TileIndex tile)
	{
		if (!this->blueprint.has_value() || !Company::IsValidID(_local_company)) return;
		int x = TileX(tile) * TILE_SIZE;
		int y = TileY(tile) * TILE_SIZE;

		if (GetBlueprintPasteStatus().active) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_BLUEPRINT_CAN_T_PASTE), GetEncodedString(STR_ERROR_BLUEPRINT_PASTE_RUNNING), WarningLevel::Info, x, y);
			return;
		}

		BlueprintPasteOptions options;
		options.transform = this->transform;
		if (_blueprint_use_selected_railtype) options.railtype = GetSelectedRailType();

		BlueprintPastePlan plan = PlanBlueprintPaste(*this->blueprint, tile, options);
		if (plan.chunks.empty()) {
			auto [w, h] = this->TransformedSize();
			EncodedString detail;
			if (this->blueprint->IsEmpty()) {
				detail = GetEncodedString(STR_ERROR_BLUEPRINT_EMPTY);
			} else if (TileX(tile) + w > Map::MaxX() || TileY(tile) + h > Map::MaxY()) {
				detail = GetEncodedString(STR_ERROR_BLUEPRINT_DOES_NOT_FIT);
			} else {
				detail = GetEncodedString(STR_ERROR_BLUEPRINT_OTHER, plan.error);
			}
			ShowErrorMessage(GetEncodedString(STR_ERROR_BLUEPRINT_CAN_T_PASTE), std::move(detail), WarningLevel::Info, x, y);
			return;
		}
		if (!StartBlueprintPaste(this->blueprint->name, std::move(plan), _local_company)) {
			ShowErrorMessage(GetEncodedString(STR_ERROR_BLUEPRINT_CAN_T_PASTE), GetEncodedString(STR_ERROR_BLUEPRINT_PASTE_RUNNING), WarningLevel::Info, x, y);
		}
		this->UpdateButtons();
		this->SetDirty();
	}

	/**
	 * Get the lines of the status panel.
	 * @return The lines, each a complete string with colours.
	 */
	std::vector<std::string> GetStatusLines() const
	{
		std::vector<std::string> lines;
		lines.push_back(GetString(STR_BLUEPRINT_STATUS_TRANSFORM, this->transform.rotation * 90, this->transform.mirror ? STR_BLUEPRINT_STATUS_MIRRORED : STR_EMPTY));
		if (this->blueprint.has_value()) {
			auto [w, h] = this->TransformedSize();
			lines.push_back(GetString(STR_BLUEPRINT_STATUS_SIZE, this->blueprint->name, w, h, this->blueprint->CountItems()));
		} else {
			lines.push_back(GetString(STR_BLUEPRINT_STATUS_NO_SELECTION));
		}

		const BlueprintPasteStatus &s = GetBlueprintPasteStatus();
		if (s.active) {
			lines.push_back(GetString(STR_BLUEPRINT_STATUS_PASTE_RUNNING, s.name, s.chunks_done, s.chunks_total, s.built, s.failed, s.cost));
		} else if (s.chunks_total != 0) {
			lines.push_back(GetString(STR_BLUEPRINT_STATUS_PASTE_DONE, s.name, s.built, s.failed, s.cost));
		} else {
			lines.push_back(GetString(STR_BLUEPRINT_STATUS_PASTE_NONE));
		}
		if (s.out_of_money) lines.push_back(GetString(STR_BLUEPRINT_STATUS_OUT_OF_MONEY));
		if (s.aborted) lines.push_back(GetString(STR_BLUEPRINT_STATUS_ABORTED));
		if (s.failed != 0 && s.last_error != INVALID_STRING_ID) lines.push_back(GetString(STR_BLUEPRINT_STATUS_LAST_ERROR, s.last_error));
		return lines;
	}

	/** Number of lines reserved for the status panel. */
	static constexpr uint STATUS_LINES = 6;

	void UpdateWidgetSize(WidgetID widget, Dimension &size, [[maybe_unused]] const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		switch (widget) {
			case WID_BP_LIST:
				fill.height = resize.height = GetCharacterHeight(FontSize::Normal) + WidgetDimensions::scaled.vsep_normal;
				size.height = std::max(size.height, padding.height + 8 * resize.height);
				break;

			case WID_BP_STATUS:
				size.height = std::max(size.height, padding.height + STATUS_LINES * GetCharacterHeight(FontSize::Normal));
				break;
		}
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_BP_LIST: {
				Rect tr = r.Shrink(WidgetDimensions::scaled.framerect);
				int line = this->resize.step_height;
				int text_offset = (line - GetCharacterHeight(FontSize::Normal) + 1) / 2;
				if (this->entries.empty()) {
					DrawString(tr.left, tr.right, tr.top + text_offset, STR_BLUEPRINT_LIST_EMPTY, TextColour::Grey);
					break;
				}
				bool rtl = _current_text_dir == TD_RTL;
				auto [first, last] = this->vscroll->GetVisibleRangeIterators(this->entries);
				for (auto it = first; it != last; ++it) {
					Entry &e = *it;
					this->LoadEntry(e);
					bool sel = e.name == this->selected;
					if (sel) GfxFillRect(r.left + WidgetDimensions::scaled.bevel.left, tr.top, r.right - WidgetDimensions::scaled.bevel.right, tr.top + line - 1, PC_DARK_GREY);

					std::string details = e.ok ? GetString(STR_BLUEPRINT_LIST_DETAILS, e.width, e.height, e.items) : GetString(STR_BLUEPRINT_LIST_BROKEN);
					int dw = GetStringBoundingBox(details).width;
					DrawString(tr.left, tr.right, tr.top + text_offset, details, e.ok ? TextColour::LightBlue : TextColour::Red, AlignmentH::End);
					Rect nr = tr.Indent(dw + WidgetDimensions::scaled.hsep_wide, !rtl);
					DrawString(nr.left, nr.right, tr.top + text_offset, e.name, sel ? TextColour::White : TextColour::Black);
					tr.top += line;
				}
				break;
			}

			case WID_BP_STATUS: {
				Rect tr = r.Shrink(WidgetDimensions::scaled.framerect);
				for (const std::string &text : this->GetStatusLines()) {
					if (tr.top + GetCharacterHeight(FontSize::Normal) > tr.bottom + 1) break;
					DrawString(tr.left, tr.right, tr.top, text);
					tr.top += GetCharacterHeight(FontSize::Normal);
				}
				break;
			}
		}
	}

	void OnResize() override
	{
		this->vscroll->SetCapacityFromWidget(this, WID_BP_LIST, WidgetDimensions::scaled.framerect.Vertical());
	}

	/**
	 * Callback of the delete confirmation.
	 * @param w The blueprint window.
	 * @param confirmed Whether the deletion was confirmed.
	 */
	static void DeleteCallback(Window *w, bool confirmed)
	{
		if (confirmed) static_cast<BlueprintWindow *>(w)->DeleteSelected();
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_BP_LIST: {
				auto it = this->vscroll->GetScrolledItemFromWidget(this->entries, pt.y, this, WID_BP_LIST, WidgetDimensions::scaled.framerect.top);
				if (it == this->entries.end()) break;
				if (it->name != this->selected) this->Select(it->name);
				if (click_count > 1 && this->mode != Mode::Paste && !this->IsWidgetDisabled(WID_BP_PASTE)) this->OnClick(pt, WID_BP_PASTE, 1);
				break;
			}

			case WID_BP_COPY:
				if (HandlePlacePushButton(this, WID_BP_COPY, SPR_CURSOR_QUERY, HT_RECT)) {
					this->mode = Mode::Copy;
				}
				this->UpdateButtons();
				break;

			case WID_BP_PASTE: {
				if (!this->blueprint.has_value()) break;
				RailType rt = GetSelectedRailType();
				if (rt == INVALID_RAILTYPE) rt = RAILTYPE_RAIL;
				if (HandlePlacePushButton(this, WID_BP_PASTE, GetRailTypeInfo(rt)->cursor.autorail, HT_RECT)) {
					this->mode = Mode::Paste;
					this->UpdatePreview();
				}
				this->UpdateButtons();
				break;
			}

			case WID_BP_RENAME:
				if (this->selected.empty()) break;
				this->query = Query::Rename;
				ShowQueryString(this->selected, STR_BLUEPRINT_QUERY_RENAME_CAPTION, MAX_LENGTH_BLUEPRINT_NAME_CHARS, this, CS_ALPHANUMERAL, {QueryStringFlag::LengthIsInChars});
				break;

			case WID_BP_DELETE:
				if (this->selected.empty()) break;
				ShowQuery(GetEncodedString(STR_BLUEPRINT_DELETE_CAPTION), GetEncodedString(STR_BLUEPRINT_DELETE_QUERY, this->selected), this, DeleteCallback);
				break;

			case WID_BP_ROTATE_LEFT:
				this->SetTransform({static_cast<uint8_t>((this->transform.rotation + 3) % 4), this->transform.mirror});
				break;

			case WID_BP_ROTATE_RIGHT:
				this->SetTransform({static_cast<uint8_t>((this->transform.rotation + 1) % 4), this->transform.mirror});
				break;

			case WID_BP_MIRROR:
				/* The mirror is applied before the rotation; to always flip along the same axis on the screen, mirroring also reverses the rotation. */
				this->SetTransform({static_cast<uint8_t>((4 - this->transform.rotation) % 4), !this->transform.mirror});
				break;

			case WID_BP_RAILTYPE:
				_blueprint_use_selected_railtype = !_blueprint_use_selected_railtype;
				this->UpdateButtons();
				this->SetDirty();
				break;

			case WID_BP_ABORT:
				AbortBlueprintPaste();
				this->UpdateButtons();
				this->SetDirty();
				break;
		}
	}

	void OnQueryTextFinished(std::optional<std::string> str) override
	{
		Query q = this->query;
		this->query = Query::None;
		switch (q) {
			case Query::NewName:
				if (str.has_value() && !str->empty()) {
					this->SaveCaptured(*str);
				} else {
					this->captured.reset();
				}
				break;

			case Query::Rename:
				if (str.has_value()) this->RenameSelected(*str);
				break;

			case Query::None:
				break;
		}
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		switch (this->mode) {
			case Mode::Copy:
				VpStartPlaceSizing(tile, VPM_X_AND_Y, DDSP_BLUEPRINT_COPY);
				break;

			case Mode::Paste:
				this->Paste(tile);
				break;

			case Mode::None:
				break;
		}
	}

	void OnPlaceDrag(ViewportPlaceMethod select_method, [[maybe_unused]] ViewportDragDropSelectionProcess select_proc, Point pt) override
	{
		VpSelectTilesWithMethod(pt.x, pt.y, select_method);
	}

	void OnPlaceMouseUp([[maybe_unused]] ViewportPlaceMethod select_method, ViewportDragDropSelectionProcess select_proc, Point pt, TileIndex start_tile, TileIndex end_tile) override
	{
		if (pt.x == -1 || select_proc != DDSP_BLUEPRINT_COPY) return;
		ResetObjectToPlace();
		this->CaptureArea(start_tile, end_tile);
	}

	void OnPlaceObjectAbort() override
	{
		this->mode = Mode::None;
		_blueprint_preview.active = false;
		this->UpdateButtons();
		this->SetDirty();
	}

	/**
	 * Some data on this window has become invalid.
	 * @param data 0: the paste job progressed; 1: blueprint files changed.
	 * @param gui_scope Whether the call is done from GUI scope.
	 */
	void OnInvalidateData(int data = 0, bool gui_scope = true) override
	{
		if (!gui_scope) return;
		if (data == 1) {
			for (Entry &e : this->entries) e.loaded = false;
			this->RebuildList();
			if (!this->selected.empty()) this->Select(std::string{this->selected});
		}
		this->UpdateButtons();
		this->SetWidgetDirty(WID_BP_STATUS);
		this->SetWidgetDirty(WID_BP_ABORT);
	}

	static inline HotkeyList hotkeys{"blueprints", {
		Hotkey('R', "rotate_right", WID_BP_ROTATE_RIGHT),
		Hotkey('R' | WKC_SHIFT, "rotate_left", WID_BP_ROTATE_LEFT),
		Hotkey('M', "mirror", WID_BP_MIRROR),
		Hotkey('C', "copy", WID_BP_COPY),
		Hotkey('V', "paste", WID_BP_PASTE),
	}};
};

static constexpr std::initializer_list<NWidgetPart> _nested_blueprint_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_BP_CAPTION), SetStringTip(STR_BLUEPRINT_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, Colours::DarkGreen),
		NWidget(WWT_DEFSIZEBOX, Colours::DarkGreen),
		NWidget(WWT_STICKYBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, Colours::DarkGreen, WID_BP_LIST), SetMinimalSize(280, 0), SetResize(1, 1), SetFill(1, 1), SetScrollbar(WID_BP_SCROLLBAR), SetToolTip(STR_BLUEPRINT_LIST_TOOLTIP), EndContainer(),
		NWidget(NWID_VSCROLLBAR, Colours::DarkGreen, WID_BP_SCROLLBAR),
	EndContainer(),
	NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BP_COPY), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_BLUEPRINT_COPY, STR_BLUEPRINT_COPY_TOOLTIP),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BP_PASTE), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_BLUEPRINT_PASTE, STR_BLUEPRINT_PASTE_TOOLTIP),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BP_RENAME), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_BLUEPRINT_RENAME, STR_BLUEPRINT_RENAME_TOOLTIP),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BP_DELETE), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_BLUEPRINT_DELETE, STR_BLUEPRINT_DELETE_TOOLTIP),
	EndContainer(),
	NWidget(NWID_HORIZONTAL, NWidContainerFlag::EqualSize),
		NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_BP_ROTATE_LEFT), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_BLUEPRINT_ROTATE_LEFT, STR_BLUEPRINT_ROTATE_LEFT_TOOLTIP),
		NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_BP_ROTATE_RIGHT), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_BLUEPRINT_ROTATE_RIGHT, STR_BLUEPRINT_ROTATE_RIGHT_TOOLTIP),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BP_MIRROR), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_BLUEPRINT_MIRROR, STR_BLUEPRINT_MIRROR_TOOLTIP),
		NWidget(WWT_TEXTBTN, Colours::DarkGreen, WID_BP_RAILTYPE), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_BLUEPRINT_RAILTYPE, STR_BLUEPRINT_RAILTYPE_TOOLTIP),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen, WID_BP_STATUS), SetResize(1, 0), SetFill(1, 0), EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PUSHTXTBTN, Colours::DarkGreen, WID_BP_ABORT), SetResize(1, 0), SetFill(1, 0), SetStringTip(STR_BLUEPRINT_ABORT, STR_BLUEPRINT_ABORT_TOOLTIP),
		NWidget(WWT_RESIZEBOX, Colours::DarkGreen),
	EndContainer(),
};

/** Window definition of the blueprint window. */
static WindowDesc _blueprint_desc(
	WindowPosition::Automatic, "blueprints", 0, 0,
	WindowClass::Blueprints, WindowClass::None,
	WindowDefaultFlag::Construction,
	_nested_blueprint_widgets,
	&BlueprintWindow::hotkeys
);

/**
 * Open the rail blueprint window, or bring it to the front.
 * @return The window.
 */
Window *ShowBlueprintWindow()
{
	return AllocateWindowDescFront<BlueprintWindow>(_blueprint_desc, 0);
}
