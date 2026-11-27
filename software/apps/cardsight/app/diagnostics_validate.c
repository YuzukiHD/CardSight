// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file diagnostics_validate.c
 * @brief Object-tree validator for the rounded-glass panel layout.
 *
 * @details Walks every visible widget and checks that it stays inside its
 * parent and inside the rounded outline of the display.
 */
#include "ui/ui_private.h"
#include "app/diagnostics.h"

/**
 * @brief Check that one rectangle is entirely inside another.
 *
 * @param[in] outer Containing area.
 * @param[in] inner Area that must fit inside @p outer.
 * @return True when every edge of @p inner lies within @p outer.
 */
static bool area_contains(const lv_area_t *outer, const lv_area_t *inner)
{
	return inner->x1 >= outer->x1 && inner->y1 >= outer->y1 &&
		inner->x2 <= outer->x2 && inner->y2 <= outer->y2;
}

/**
 * @brief Check whether a point lies inside the rounded screen glass.
 *
 * @details Is the point inside the rounded glass? Only the four corner
 * squares can fail. The distance from the nearest corner centre is compared
 * with the radius squared; the outside region is clamped away first so the
 * test is exact for points near the middle as well.
 *
 * @param[in] x_pos Horizontal position in screen coordinates.
 * @param[in] y_pos Vertical position in screen coordinates.
 * @return True when the point is on the glass.
 */
static bool point_on_glass(int32_t x_pos, int32_t y_pos)
{
	int32_t radius = ui.screen_radius;
	int32_t dx = x_pos < radius ? radius - x_pos : x_pos > (int32_t)ui.width - 1 - radius ?
		x_pos - ((int32_t)ui.width - 1 - radius) : 0;
	int32_t dy = y_pos < radius ? radius - y_pos : y_pos > (int32_t)ui.height - 1 - radius ?
		y_pos - ((int32_t)ui.height - 1 - radius) : 0;

	return dx * dx + dy * dy <= radius * radius;
}

/**
 * @brief Check the four corners of a rounded object against the glass.
 *
 * @details A rounded object only reaches its bounding corner along the arc;
 * test the 45 degree point of it. The radius is capped at half the shorter
 * side because LVGL caps it there too, and the inset is measured from the
 * bounding corner to that 45 degree point.
 *
 * @param[in] object Object whose radius applies.
 * @param[in] area   Bounding box of the object.
 * @return True when all four arc corners are on the glass.
 */
static bool corners_on_glass(lv_obj_t *object, const lv_area_t *area)
{
	int32_t radius = MIN(lv_obj_get_style_radius(object, LV_PART_MAIN),
		MIN(lv_area_get_width(area), lv_area_get_height(area)) / 2);
	int32_t inset = radius - radius * 7071 / 10000;

	return point_on_glass(area->x1 + inset, area->y1 + inset) &&
		point_on_glass(area->x2 - inset, area->y1 + inset) &&
		point_on_glass(area->x1 + inset, area->y2 - inset) &&
		point_on_glass(area->x2 - inset, area->y2 - inset);
}

/**
 * @brief Walk a widget tree and count layout problems.
 *
 * @details Hidden children are skipped. A child must lie inside its parent
 * unless the parent scrolls or clips its corners, and a child must stay on the
 * glass unless an ancestor already clipped it. Clickable children must be at
 * least APP_TOUCH_MIN along one axis and never smaller than 32x32, because the
 * tab bar and the format segments are as large as the short side allows.
 * Every failure is logged with the page name and the offending child, and the
 * walk recurses with the clipping state accumulated so far.
 *
 * @param[in]     parent   Parent whose children are checked.
 * @param[in]     page     Page name used in the log.
 * @param[in]     clipped  True when an ancestor already restricts its children.
 * @param[in,out] failures Counter receiving one per problem.
 */
static void validate_tree(lv_obj_t *parent, const char *page, bool clipped, int *failures)
{
	lv_area_t parent_area;
	bool scrollable = lv_obj_is_scrollable(parent);
	/* Glass highlights are a full-screen backdrop image clipped by their rounded parent, so a
	 * clipping parent legitimately holds children that reach past its own box. */
	bool clips = lv_obj_get_style_clip_corner(parent, LV_PART_MAIN);
	uint32_t children = lv_obj_get_child_count(parent);

	lv_obj_get_coords(parent, &parent_area);
	for (uint32_t index = 0U; index < children; index++) {
		lv_obj_t *child = lv_obj_get_child(parent, (int32_t)index);
		lv_area_t child_area;

		if (lv_obj_is_hidden(child)) {
			continue;
		}
		lv_obj_get_coords(child, &child_area);
		if (!scrollable && !clips && !area_contains(&parent_area, &child_area)) {
			pr_err("UI test %s: child %u outside parent (%ld,%ld)-(%ld,%ld)\n", page,
				(uint32_t)index, (long)child_area.x1, (long)child_area.y1,
				(long)child_area.x2, (long)child_area.y2);
			(*failures)++;
		}
		if (!clipped && !scrollable && !clips && !lv_obj_is_scrollable(child) &&
			!corners_on_glass(child, &child_area)) {
			pr_err("UI test %s: child %u leaves the rounded glass r=%ld (%ld,%ld)-(%ld,%ld)\n", page,
				(uint32_t)index, (long)lv_obj_get_style_radius(child, LV_PART_MAIN), (long)child_area.x1, (long)child_area.y1,
				(long)child_area.x2, (long)child_area.y2);
			(*failures)++;
		}
		if (lv_obj_is_clickable(child)) {
			int32_t width = lv_obj_get_width(child);
			int32_t height = lv_obj_get_height(child);
			/* The tab bar (42 px) and the format segments (36 px) are as large as the short
			 * side allows; what matters is a comfortable long side and no tiny target. */
			bool comfortable = width >= APP_TOUCH_MIN || height >= APP_TOUCH_MIN;

			if (!comfortable || width < 32 || height < 32) {
				pr_err("UI test %s: clickable child %u is %ldx%ld\n", page, (uint32_t)index,
					(long)width, (long)height);
				(*failures)++;
			}
		}
		validate_tree(child, page, clipped || scrollable || clips, failures);
	}
}

/**
 * @brief Validate a whole widget tree with no ancestor clipping.
 *
 * @param[in]     parent   Root of the tree to check.
 * @param[in]     page     Page name used in the log.
 * @param[in,out] failures Counter receiving one per problem.
 */
static void validate_object_tree(lv_obj_t *parent, const char *page, int *failures)
{
	validate_tree(parent, page, false, failures);
}

/**
 * @brief Validate the currently shown page and the bottom bar.
 *
 * @details The layout is updated first so the coordinates are final. The tile
 * of the current mode must exist together with the bottom bar; after that both
 * trees are walked independently.
 *
 * @param[in]     name     Page name used in the log.
 * @param[in,out] failures Counter receiving one per problem.
 */
void validate_active_page(const char *name, int *failures)
{
	lv_obj_t *active = ui.tiles[(uint32_t)ui.mode];

	lv_obj_update_layout(ui.screen);
	if (active == NULL || ui.bar == NULL) {
		pr_err("UI test %s: no active tile or bottom bar\n", name);
		(*failures)++;
		return;
	}
	validate_object_tree(active, name, failures);
	validate_object_tree(ui.bar, name, failures);
}

/**
 * @brief Validate the active page, the format dialog and the banner.
 *
 * @return Number of problems found; zero when the UI passes.
 */
int cardsight_validate_ui(void)
{
	int failures = 0;
	validate_active_page("active", &failures);
	if (ui.format_dialog != NULL) validate_object_tree(ui.format_dialog, "format", &failures);
	if (ui.banner != NULL) validate_object_tree(ui.banner, "banner", &failures);
	return failures;
}
