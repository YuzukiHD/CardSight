// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file ui_nav.c
 * @brief Tab bar, page switching and the bottom action bar.
 *
 * @details Part of the CardSight UI. The widget tree and view model live in
 * @c ui_private.h; pages compose their trees from the helpers in
 * @c ui/widgets/widgets.h.
 */
#include "ui/ui_private.h"

/** @brief Inset of the tab highlight and buttons from the bar edge in pixels. */
#define TAB_PAD 2

/** @brief Page whose tab is currently highlighted; drives the slide animation. */
static enum cardsight_mode tab_shown;


/**
 * @brief Return the width of the bottom bar inside the screen margins.
 *
 * @return Width in pixels.
 */
int32_t bar_width(void)
{
	return (int32_t)ui.width - 2 * APP_BAR_MARGIN;
}

/**
 * @brief Return the width of one tab cell.
 *
 * @details Splits the bar's inner width, after the two-pixel border and the
 *          pads, evenly across the tabs.
 *
 * @return Width in pixels.
 */
int32_t tab_width(void)
{
	return (bar_width() - 2 - 2 * TAB_PAD) / (int32_t)APP_TAB_COUNT;
}

/* The five tabs share the inner width; the last one is pinned to the right pad so the
 * outer gaps match instead of leaving the division remainder on the right. */
/**
 * @brief Return the X position of a tab inside the bar.
 *
 * @details Distributes the free space evenly between the first and last tab and
 *          pins the last tab to the right pad, so the outer gaps match instead
 *          of leaving the division remainder on the right.
 *
 * @param[in] index Tab index.
 * @return X position in pixels.
 */
int32_t tab_x(uint32_t index)
{
	int32_t inner = bar_width() - 2 - 2 * TAB_PAD;
	int32_t width = tab_width();
	int32_t step = (inner - width) / (int32_t)(APP_TAB_COUNT - 1U);

	if (index + 1U >= APP_TAB_COUNT) {
		return TAB_PAD + inner - width;
	}
	return TAB_PAD + (int32_t)index * step;
}

/**
 * @brief Animation callback that slides the tab highlight.
 *
 * @details Moves the indicator and then realigns its glass backdrop so the
 *          blurred wallpaper stays registered while the highlight moves.
 *
 * @param[in,out] object Indicator object.
 * @param[in]     value  New X position in pixels.
 */
void tab_anim_x(void *object, int32_t value)
{
	lv_obj_set_x(object, value);
	glass_highlight_move(ui.tab_indicator, ui.tab_indicator_backdrop);
}

/* The selected icon grows a little; the move is eased so it reads as one motion with the
 * sliding highlight. */
/**
 * @brief Animation callback that scales a tab icon.
 *
 * @param[in,out] object Icon being scaled.
 * @param[in]     value  New LVGL image scale (256 is 1:1).
 */
void tab_icon_anim_scale(void *object, int32_t value)
{
	lv_image_set_scale(object, (uint32_t)value);
}

/**
 * @brief Animate a tab icon to a target scale.
 *
 * @details Returns early when the icon is already at the target. Without
 *          animations the scale is set directly; otherwise it eases over
 *          160 ms so the growth reads as one motion with the sliding highlight.
 *
 * @param[in,out] icon   Icon to scale; NULL is ignored.
 * @param[in]     target Target LVGL image scale (256 is 1:1).
 */
void tab_icon_scale_to(lv_obj_t *icon, int32_t target)
{
	lv_anim_t animation;

	if (icon == NULL || lv_image_get_scale(icon) == target) {
		return;
	}
	if (!ui.animations) {
		lv_image_set_scale(icon, (uint32_t)target);
		return;
	}
	lv_anim_init(&animation);
	lv_anim_set_var(&animation, icon);
	lv_anim_set_exec_cb(&animation, tab_icon_anim_scale);
	lv_anim_set_values(&animation, lv_image_get_scale(icon), target);
	lv_anim_set_duration(&animation, 160);
	lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
	lv_anim_start(&animation);
}

/* Slide the highlight to the active tab and recolour the labels. */
/**
 * @brief Slide the highlight to the active tab and recolour the labels.
 *
 * @details Every tab is coloured by selection and the selected icon is scaled
 *          up slightly. The highlight moves with an eased animation, unless the
 *          page did not change or animations are disabled, in which case it
 *          jumps to the target.
 */
void ui_update_tabs(void)
{
	int32_t target;

	if (ui.tabbar == NULL) {
		return;
	}
	target = tab_x((uint32_t)ui.mode);
	for (uint32_t index = 0U; index < ARRAY_SIZE(ui.tab_buttons); index++) {
		bool selected = ui.mode == (enum cardsight_mode)index;
		uint32_t color = selected ? UI_TEXT : UI_MUTED;

		lv_obj_set_style_text_color(ui.tab_buttons[index], lv_color_hex(color), 0);
		if (ui.tab_icons[index] != NULL) {
			lv_obj_set_style_image_recolor(ui.tab_icons[index], lv_color_hex(color), 0);
			lv_obj_set_style_image_recolor_opa(ui.tab_icons[index], LV_OPA_COVER, 0);
			tab_icon_scale_to(ui.tab_icons[index], selected ? 288 : 256);
		}
	}
	if (tab_shown == ui.mode || !ui.animations) {
		lv_anim_delete(ui.tab_indicator, tab_anim_x);
		lv_obj_set_x(ui.tab_indicator, target);
		glass_highlight_move(ui.tab_indicator, ui.tab_indicator_backdrop);
		tab_shown = ui.mode;
		return;
	}
	tab_shown = ui.mode;
	lv_anim_t animation;

	lv_anim_init(&animation);
	lv_anim_set_var(&animation, ui.tab_indicator);
	lv_anim_set_exec_cb(&animation, tab_anim_x);
	lv_anim_set_values(&animation, lv_obj_get_x(ui.tab_indicator), target);
	lv_anim_set_duration(&animation, 160);
	lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
	lv_anim_start(&animation);
}

/**
 * @brief Click handler for a bottom tab.
 *
 * @details Does nothing while a request is busy, because leaving its page would
 *          hide its progress. Otherwise the tab's index, carried as user data,
 *          selects the page.
 *
 * @param[in] event LVGL click event carrying the tab index.
 */
void tab_event(lv_event_t *event)
{
	/* A running request owns the screen; leaving its page would hide its progress. */
	if (ui.model.busy) {
		return;
	}
	show_page((enum cardsight_mode)(uintptr_t)lv_event_get_user_data(event));
}

/* Five icon tabs along the bottom; the outer pill follows the glass corners. */
/**
 * @brief Build the five-icon tab bar along the bottom.
 *
 * @details Wraps the bar in a glass card, then creates the sliding highlight
 *          with its own frosted backdrop, followed by one transparent ghost
 *          button per tab holding a scaled, recoloured icon. The buttons are
 *          disabled while a request is busy.
 */
void build_tab_bar(void)
{
	/** @brief Icon of each tab, indexed like the pages. */
	static const lv_image_dsc_t *const icons[APP_TAB_COUNT] = {
		&cardsight_icon_usb, &cardsight_icon_proxy, &cardsight_icon_drive,
		&cardsight_icon_folder, &cardsight_icon_info,
	};
	int32_t width = tab_width();
	/* Children are placed inside the 1 px border, so the highlight margin is measured from there. */
	int32_t inner_height = APP_BAR_HEIGHT - 2 - 2 * TAB_PAD;
	int32_t inner_radius = MAX(ui_bar_radius() - TAB_PAD, 8);

	ui.tabbar = make_glass_card(ui.bar, 0, 0, bar_width(), APP_BAR_HEIGHT, ui_bar_radius());
	ui.tab_indicator = lv_obj_create(ui.tabbar);
	lv_obj_set_size(ui.tab_indicator, width, inner_height);
	lv_obj_set_pos(ui.tab_indicator, tab_x((uint32_t)ui.mode), TAB_PAD);
	ui.tab_indicator_backdrop = glass_highlight_attach(ui.tab_indicator, inner_radius);
	tab_shown = ui.mode;
	for (uint32_t index = 0U; index < ARRAY_SIZE(icons); index++) {
		lv_obj_t *tab = lv_button_create(ui.tabbar);
		lv_obj_t *icon;

		lv_obj_set_size(tab, width, inner_height);
		lv_obj_set_pos(tab, tab_x(index), TAB_PAD);
		ui_style_button(tab, UI_BUTTON_GHOST);
		lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, LV_STATE_PRESSED);
		lv_obj_set_style_border_width(tab, 0, 0);
		lv_obj_set_style_radius(tab, inner_radius, 0);
		lv_obj_add_event_cb(tab, tab_event, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
		icon = lv_image_create(tab);
		lv_image_set_src(icon, icons[index]);
		lv_obj_center(icon);
		lv_obj_set_style_image_recolor(icon, lv_color_hex(UI_MUTED), 0);
		lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
		lv_image_set_scale(icon, 256U);
		ui.tab_icons[index] = icon;
		ui.tab_buttons[index] = tab;
		disable_scroll(tab);
		set_disabled(tab, ui.model.busy);
	}
}

/**
 * @brief Common handler for the back gesture or button.
 *
 * @details Ignored while busy. On the Files page in detail view it navigates up
 *          one directory; elsewhere it leaves the layout editor or partition
 *          detail view and schedules a rebuild.
 *
 * @param[in] event LVGL event that triggered the back action.
 */
void ui_back_event(lv_event_t *event)
{
	(void)event;
	if (ui.model.busy) {
		return;
	}
	if (ui.mode == CARDSIGHT_FILES && ui.files_detail) {
		files_go_back();
		return;
	}
	ui.layout_editor = false;
	ui.manage_detail = false;
	ui_schedule_rebuild();
}

/* Sub pages swap the tabs for a full width back button. */
/**
 * @brief Build the bottom bar: layout actions, a back button, or the tabs.
 *
 * @details The bar is a transparent box that lets the glass card's shadow spill
 *          outside it. It hosts the layout editor actions, a full-width back
 *          button on detail pages, and the tab bar otherwise.
 */
void build_bar(void)
{
	ui.bar = make_box(ui.screen, bar_width(), APP_BAR_HEIGHT);
	lv_obj_set_pos(ui.bar, APP_BAR_MARGIN, (int32_t)ui.height - APP_BAR_MARGIN - APP_BAR_HEIGHT);
	/* the glass card's shadow lives outside the bar; the screen still clips the outside */
	lv_obj_set_overflow_visible(ui.bar, true);
	if (ui.layout_editor) {
		build_layout_bar(ui.bar);
	} else if (ui.manage_detail || (ui.mode == CARDSIGHT_FILES && ui.files_detail)) {
		make_bar_button(ui.bar, "Back", ui_back_event, bar_width());
	} else {
		build_tab_bar();
	}
}

/**
 * @brief Switch the visible page and update the tab bar.
 *
 * @details Hides every other tile, leaves any open detail view, points the
 *          content host at the selected tile, and refreshes the tabs, header,
 *          and backdrop.
 *
 * @param[in] mode Page to show.
 */
void show_page(enum cardsight_mode mode)
{
	for (uint32_t index = 0U; index < ARRAY_SIZE(ui.tiles); index++) {
		lv_obj_set_hidden(ui.tiles[index], index != (uint32_t)mode);
	}
	ui.mode = mode;
	if (ui.layout_editor || ui.manage_detail) {
		ui.layout_editor = false;
		ui.manage_detail = false;
		ui_schedule_rebuild();
	}
	ui.content = ui.tiles[(uint32_t)mode];
	ui_update_tabs();
	ui_update_header();
	ui_update_backdrop();
}
