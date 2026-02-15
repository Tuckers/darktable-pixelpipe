#!/usr/bin/env python3
"""
strip_iop.py - Strip GUI-related code from darktable IOP .c files.

Usage: python strip_iop.py input.c output.c

This script removes GUI code from darktable IOP source files while preserving
all image processing logic, parameter structs, and pipeline functions.

The script is conservative: when uncertain, it keeps the code and adds a
TODO comment.
"""

import os
import re
import sys

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

GUI_INCLUDE_PATTERNS = [
    re.compile(r'"bauhaus/bauhaus\.h"'),
    re.compile(r'"dtgtk/[^"]*"'),
    re.compile(r'"gui/[^"]*"'),
    re.compile(r"<gtk/gtk\.h>"),
    re.compile(r'"develop/imageop_gui\.h"'),
    re.compile(r'[<"][^"<>]*gui[^"<>]*[>"]', re.IGNORECASE),
]

GUI_FUNCTION_NAMES = {
    "gui_init",
    "gui_cleanup",
    "gui_update",
    "gui_changed",
    "gui_reset",
    "gui_focus",
    "color_picker_apply",
    "mouse_moved",
    "mouse_pressed",
    "mouse_released",
    "mouse_scrolled",
    "mouse_leave",
    "mouse_actions",
}

GUI_FUNCTION_PREFIXES = ["gui_"]

KEEP_FUNCTIONS = {
    "process",
    "process_cl",
    "init",
    "cleanup",
    "commit_params",
    "init_pipe",
    "cleanup_pipe",
    "init_global",
    "cleanup_global",
    "modify_roi_in",
    "modify_roi_out",
    "distort_transform",
    "distort_backtransform",
    "distort_mask",
    "name",
    "aliases",
    "description",
    "default_group",
    "flags",
    "default_colorspace",
    "input_colorspace",
    "output_colorspace",
    "blend_colorspace",
    "legacy_params",
    "init_presets",
    "reload_defaults",
    "tiling_callback",
}

GUI_TOKENS = [
    "_gui_data_t",
    "GtkWidget",
    "GtkStack",
    "GtkLabel",
    "GtkToggleButton",
    "GtkDrawingArea",
    "GtkNotebook",
    "GtkAllocation",
    "GtkStyleContext",
    "GtkColorButton",
    "GtkBin",
    "GTK_WIDGET",
    "GTK_LABEL",
    "GTK_STACK",
    "GTK_BIN",
    "GTK_BUTTON",
    "GTK_IS_GESTURE",
    "GTK_TOGGLE_BUTTON",
    "GTK_DRAWING_AREA",
    "gtk_widget_",
    "gtk_label_",
    "gtk_box_",
    "gtk_stack_",
    "gtk_toggle_",
    "gtk_button_",
    "gtk_drawing_area_",
    "gtk_notebook_",
    "dt_bauhaus_",
    "darktable.gui",
    "darktable.bauhaus",
    "dt_iop_gui_",
    "IOP_GUI_ALLOC",
    "g_signal_connect",
    "g_idle_add",
    "g_idle_remove",
    "DT_PIXEL_APPLY_DPI",
    "DT_BAUHAUS_",
    "PANGO_ELLIPSIZE",
    "PangoRectangle",
    "pango_",
    "cairo_set_source",
    "cairo_rectangle",
    "cairo_fill",
    "cairo_stroke",
    "cairo_destroy",
    "cairo_create",
    "cairo_paint",
    "cairo_set_line",
    "cairo_surface_",
    "dt_cairo_image_surface",
    "dt_action_widget_toast",
    "dt_shortcut_register",
    "dt_dev_add_history_item",
    "dt_color_picker_new",
    "dt_iop_color_picker_reset",
    "dt_gui_update_collapsible",
    "dt_gui_new_collapsible",
    "dt_gui_box_add",
    "dt_gui_hbox",
    "dt_gui_vbox",
    "dt_gui_expand",
    "dt_ui_label_new",
    "dt_ui_section_label_new",
]


# ---------------------------------------------------------------------------
# Brace-matching helpers
# ---------------------------------------------------------------------------


def find_brace_block_end(lines, start_idx):
    """Find the closing brace of a block starting from start_idx."""
    depth = 0
    found_open = False
    i = start_idx
    while i < len(lines):
        stripped = _strip_strings_and_chars(lines[i])
        comment_pos = stripped.find("//")
        if comment_pos >= 0:
            stripped = stripped[:comment_pos]
        for ch in stripped:
            if ch == "{":
                depth += 1
                found_open = True
            elif ch == "}":
                depth -= 1
                if found_open and depth == 0:
                    return i
        i += 1
    return None


def _strip_strings_and_chars(line):
    """Remove string and char literals so brace counting isn't fooled."""
    result = []
    i = 0
    while i < len(line):
        ch = line[i]
        if ch == '"':
            i += 1
            while i < len(line):
                if line[i] == "\\":
                    i += 2
                    continue
                if line[i] == '"':
                    i += 1
                    break
                i += 1
            continue
        elif ch == "'":
            i += 1
            while i < len(line):
                if line[i] == "\\":
                    i += 2
                    continue
                if line[i] == "'":
                    i += 1
                    break
                i += 1
            continue
        result.append(ch)
        i += 1
    return "".join(result)


# ---------------------------------------------------------------------------
# Line classifiers
# ---------------------------------------------------------------------------


def is_gui_include(line):
    stripped = line.strip()
    if not stripped.startswith("#include"):
        return False
    for pattern in GUI_INCLUDE_PATTERNS:
        if pattern.search(stripped):
            return True
    return False


def is_gui_data_struct_start(line):
    return bool(re.search(r"\btypedef\s+struct\s+dt_iop_\w+_gui_data_t\b", line))


# ---------------------------------------------------------------------------
# Function detection
# ---------------------------------------------------------------------------

FUNC_DEF_RE = re.compile(
    r"^(?P<qualifiers>(?:static\s+|const\s+|inline\s+|DT_OMP_\w+\s+)*)"
    r"(?P<return_type>(?:(?:unsigned|signed|long|short|struct|enum|const|volatile)\s+)*"
    r"(?:void|int|float|double|gboolean|gchar|char|size_t|cl_int|"
    r"dt_\w+|GtkWidget|cairo_\w+)\s*\*?\s*)"
    r"(?P<name>[a-zA-Z_]\w*)\s*\("
)

FORWARD_DECL_RE = re.compile(r"^static\s+[\w\s\*]+\b([a-zA-Z_]\w*)\s*\([^)]*\)\s*;")

NOT_FUNCTIONS = {
    "if",
    "for",
    "while",
    "switch",
    "return",
    "sizeof",
    "typeof",
    "DT_OMP_FOR",
    "DT_OMP_FOR_SIMD",
    "DT_OMP_PRAGMA",
}


def is_forward_declaration(line):
    """Check if a line is a forward declaration (ends with ; after params)."""
    stripped = line.strip()
    # Forward declarations end with );
    return bool(re.match(r".*\)\s*;$", stripped))


def get_function_name(line):
    """Extract function name from a function definition line."""
    stripped = line.strip()
    if (
        not stripped
        or stripped.startswith("#")
        or stripped.startswith("//")
        or stripped.startswith("/*")
    ):
        return None, None

    m = FUNC_DEF_RE.match(stripped)
    if m:
        name = m.group("name")
        is_static = "static" in m.group("qualifiers")
        if name in NOT_FUNCTIONS:
            return None, None
        return name, is_static
    return None, None


def is_explicitly_gui_function(name):
    if name in GUI_FUNCTION_NAMES:
        return True
    for prefix in GUI_FUNCTION_PREFIXES:
        if name.startswith(prefix):
            return True
    return False


def function_body_has_gui_tokens(func_body):
    for token in GUI_TOKENS:
        if token in func_body:
            return True
    return False


# ---------------------------------------------------------------------------
# Build function map (skipping forward declarations)
# ---------------------------------------------------------------------------


def build_function_map(lines):
    """Build a map of function definitions: name -> (start, end, is_static).

    Skips forward declarations (lines ending with ';' after closing paren).
    """
    func_map = {}
    i = 0
    while i < len(lines):
        name, is_static = get_function_name(lines[i])
        if name:
            # Check if this is a forward declaration
            if is_forward_declaration(lines[i]):
                i += 1
                continue
            # Also check multi-line forward declarations: if the closing
            # paren and semicolon are on the next line
            if i + 1 < len(lines) and is_forward_declaration(lines[i] + lines[i + 1]):
                i += 1
                continue
            end = find_brace_block_end(lines, i)
            if end is not None:
                func_map[name] = (i, end, is_static)
                i = end + 1
                continue
        i += 1
    return func_map


def build_call_graph(lines, func_map):
    """For each function, find which other defined functions it references."""
    calls = {name: set() for name in func_map}
    for caller_name, (start, end, _) in func_map.items():
        body = "\n".join(lines[start : end + 1])
        for callee_name in func_map:
            if callee_name == caller_name:
                continue
            if re.search(r"\b" + re.escape(callee_name) + r"\b", body):
                calls[caller_name].add(callee_name)
    return calls


# ---------------------------------------------------------------------------
# Find all references to a function name (excluding its own body, forward
# declarations, and bodies of functions in skip_set)
# ---------------------------------------------------------------------------


def has_external_ref(name, lines, func_map, skip_set):
    """Check if name is referenced outside its own body and skip_set bodies."""
    if name not in func_map:
        return True  # conservative: if we can't find it, assume it's used
    start, end, _ = func_map[name]
    pattern = re.compile(r"\b" + re.escape(name) + r"\b")

    for j, line in enumerate(lines):
        if start <= j <= end:
            continue
        # Skip inside functions that are in skip_set
        in_skip = False
        for sn in skip_set:
            if sn in func_map:
                ss, se, _ = func_map[sn]
                if ss <= j <= se:
                    in_skip = True
                    break
        if in_skip:
            continue
        # Skip forward declarations
        if FORWARD_DECL_RE.match(line.strip()):
            fwd = FORWARD_DECL_RE.match(line.strip())
            if fwd and fwd.group(1) == name:
                continue
        if pattern.search(line):
            return True
    return False


# ---------------------------------------------------------------------------
# Iterative function removal
# ---------------------------------------------------------------------------


def find_all_functions_to_remove(lines, func_map):
    """Find all functions to remove via iterative analysis.

    Phase 1: Seed with explicitly listed GUI functions
    Phase 2: Iteratively remove static GUI-token functions with no
             external references outside already-removed code
    Phase 3: Handle cycles of GUI-token functions
    Phase 4: Remove orphaned static functions (no GUI tokens but only
             referenced by removed code)
    """
    to_remove = set()

    # Phase 1: explicit GUI functions
    for name in func_map:
        if name in KEEP_FUNCTIONS:
            continue
        if is_explicitly_gui_function(name):
            to_remove.add(name)

    # Phase 2: iterative removal of GUI-token static functions
    changed = True
    while changed:
        changed = False
        for name, (start, end, is_static) in func_map.items():
            if name in to_remove or name in KEEP_FUNCTIONS or not is_static:
                continue
            body = "\n".join(lines[start : end + 1])
            if not function_body_has_gui_tokens(body):
                continue
            if not has_external_ref(name, lines, func_map, to_remove):
                to_remove.add(name)
                changed = True

    # Phase 3: cycle detection for remaining GUI-token static functions
    remaining_gui = set()
    for name, (start, end, is_static) in func_map.items():
        if name in to_remove or name in KEEP_FUNCTIONS or not is_static:
            continue
        body = "\n".join(lines[start : end + 1])
        if function_body_has_gui_tokens(body):
            remaining_gui.add(name)

    if remaining_gui:
        # For each member, check if it has references outside
        # (to_remove ∪ remaining_gui)
        combined_skip = to_remove | remaining_gui
        can_remove = set()
        for name in remaining_gui:
            if not has_external_ref(name, lines, func_map, combined_skip):
                can_remove.add(name)
        # Only remove the cycle if ALL members can be removed
        # (i.e., none are referenced by kept code)
        if can_remove == remaining_gui:
            to_remove.update(can_remove)
        else:
            # Remove only the ones that truly have no external refs
            to_remove.update(can_remove)

    # Phase 4: iteratively remove orphaned static functions
    # (even without GUI tokens, if only referenced by removed code)
    changed = True
    while changed:
        changed = False
        for name, (start, end, is_static) in func_map.items():
            if name in to_remove or name in KEEP_FUNCTIONS or not is_static:
                continue
            if not has_external_ref(name, lines, func_map, to_remove):
                to_remove.add(name)
                changed = True

    return to_remove


# ---------------------------------------------------------------------------
# Stripping gui_data refs in kept functions
# ---------------------------------------------------------------------------


def strip_gui_data_in_function(lines, func_start, func_end):
    """Strip gui_data references within a specific function."""
    result = []
    gui_data_var = None
    i = func_start

    while i <= func_end:
        line = lines[i]

        # Detect gui_data variable assignment
        gui_data_assign = re.match(
            r"(\s*)(?:const\s+)?(?:dt_iop_\w+_gui_data_t|void)\s*\*\s*"
            r"(?:const\s+)?(\w+)\s*=\s*"
            r"(?:\([^)]*\)\s*)?self->gui_data\s*;",
            line,
        )
        if gui_data_assign:
            gui_data_var = gui_data_assign.group(2)
            i += 1
            continue

        # Pattern: ((dt_iop_XXX_gui_data_t *)self->gui_data)->field = value;
        if re.match(
            r"\s*\(\(dt_iop_\w+_gui_data_t\s*\*\)\s*self->gui_data\)->\w+\s*=.*;\s*$",
            line,
        ):
            i += 1
            continue

        # Pattern: if(self->gui_data) one-liner
        if re.match(r"\s*if\s*\(\s*self->gui_data\s*\)\s*\{[^}]*\}\s*$", line):
            i += 1
            continue

        # Pattern: if(self->gui_data) multi-line block
        if re.match(r"\s*if\s*\(\s*self->gui_data\s*\)", line):
            block_end = find_brace_block_end(lines, i)
            if block_end is not None and block_end <= func_end:
                i = block_end + 1
                continue

        if gui_data_var:
            # Pattern: if(g) { ... } else { ... } -> keep only else body
            g_if_match = re.match(
                r"(\s*)if\s*\(\s*" + re.escape(gui_data_var) + r"\s*\)",
                line,
            )
            if g_if_match:
                indent = g_if_match.group(1)
                if_block_end = find_brace_block_end(lines, i)
                if if_block_end is not None and if_block_end <= func_end:
                    # Look for else clause
                    next_i = if_block_end + 1
                    while next_i <= func_end and lines[next_i].strip() == "":
                        next_i += 1

                    has_else = next_i <= func_end and re.match(
                        r"\s*else\b", lines[next_i]
                    )

                    if has_else:
                        else_block_end = find_brace_block_end(lines, next_i)
                        if else_block_end is not None and else_block_end <= func_end:
                            else_open = next_i
                            while else_open <= else_block_end:
                                if "{" in lines[else_open]:
                                    break
                                else_open += 1
                            for k in range(else_open + 1, else_block_end):
                                eline = lines[k]
                                if eline.startswith(indent + "    "):
                                    eline = indent + eline[len(indent) + 4 :]
                                elif eline.startswith(indent + "  "):
                                    eline = indent + eline[len(indent) + 2 :]
                                result.append(eline)
                            i = else_block_end + 1
                            continue

                    # No else - just remove if(g) block
                    i = if_block_end + 1
                    continue

            # Pattern: if(g && ...) { ... }
            if re.match(
                r"\s*if\s*\(\s*" + re.escape(gui_data_var) + r"\s*&&",
                line,
            ):
                block_end = find_brace_block_end(lines, i)
                if block_end is not None and block_end <= func_end:
                    i = block_end + 1
                    continue

            # Pattern: if(... && g ...) { ... }
            if re.match(
                r"\s*if\s*\(.*&&\s*" + re.escape(gui_data_var) + r"\b",
                line,
            ):
                block_end = find_brace_block_end(lines, i)
                if block_end is not None and block_end <= func_end:
                    i = block_end + 1
                    continue

            # Single-line: g->field = ...;
            if re.match(
                r"\s*" + re.escape(gui_data_var) + r"->\w+\s*=.*;\s*$",
                line,
            ):
                i += 1
                continue

        result.append(line)
        i += 1

    return result


# ---------------------------------------------------------------------------
# Forward declaration cleanup
# ---------------------------------------------------------------------------


def remove_forward_declarations(lines, removed_names):
    result = []
    for line in lines:
        m = FORWARD_DECL_RE.match(line.strip())
        if m and m.group(1) in removed_names:
            continue
        result.append(line)
    return result


# ---------------------------------------------------------------------------
# Main stripping logic
# ---------------------------------------------------------------------------


def strip_iop(input_path, output_path):
    with open(input_path, "r") as f:
        lines = f.readlines()

    # --- Pass 1: Build function map and find functions to remove ---
    func_map = build_function_map(lines)
    to_remove = find_all_functions_to_remove(lines, func_map)

    # --- Pass 2: Remove includes, structs, and functions ---
    output_lines = []
    i = 0
    while i < len(lines):
        line = lines[i]

        # Remove GUI includes
        if is_gui_include(line):
            i += 1
            continue

        # Remove commented-out GUI includes
        stripped = line.strip()
        if stripped.startswith("//") and "#include" in stripped:
            test_line = stripped.lstrip("/").strip()
            if is_gui_include(test_line):
                i += 1
                continue

        # Remove gui_data_t struct definitions
        if is_gui_data_struct_start(line):
            end = find_brace_block_end(lines, i)
            if end is not None:
                i = end + 1
                continue

        # Remove functions
        name, is_static = get_function_name(line)
        if name and name in to_remove and name not in KEEP_FUNCTIONS:
            # Skip forward declarations (they're handled in Pass 3)
            if not is_forward_declaration(line):
                end = find_brace_block_end(lines, i)
                if end is not None:
                    i = end + 1
                    continue

        output_lines.append(line)
        i += 1

    # --- Pass 3: Remove forward declarations of removed functions ---
    output_lines = remove_forward_declarations(output_lines, to_remove)

    # --- Pass 4: Strip gui_data references in all remaining functions ---
    kept_func_map = build_function_map(output_lines)
    funcs_to_strip = []
    for name, (start, end, is_static) in kept_func_map.items():
        body = "\n".join(output_lines[start : end + 1])
        if "gui_data" in body or "_gui_data_t" in body:
            funcs_to_strip.append((name, start, end))

    if funcs_to_strip:
        # Process in reverse order so line indices remain valid
        funcs_to_strip.sort(key=lambda x: x[1], reverse=True)
        for name, start, end in funcs_to_strip:
            stripped_body = strip_gui_data_in_function(output_lines, start, end)
            output_lines[start : end + 1] = stripped_body

    # --- Pass 4b: Remove self->widget and gtk_stack GUI lines in kept funcs ---
    clean_widget = []
    for line in output_lines:
        stripped = line.strip()
        # Remove: if(self->widget) gtk_stack_set_visible_child_name(...)
        if re.match(r"if\s*\(\s*self->widget\s*\)", stripped):
            if "gtk_stack" in stripped or "gtk_widget" in stripped:
                clean_widget.append("")  # keep blank for spacing
                continue
        # Remove standalone gtk_stack/gtk_widget calls on self->widget
        if "self->widget" in stripped and (
            "gtk_stack_" in stripped or "gtk_widget_" in stripped
        ):
            continue
        clean_widget.append(line)
    output_lines = clean_widget

    # --- Pass 5: Remove orphaned functions in the stripped output ---
    # After gui_data stripping removed call sites, some functions may
    # now be unreferenced.
    post_func_map = build_function_map(output_lines)
    post_orphans = set()
    changed = True
    while changed:
        changed = False
        for name, (start, end, is_static) in post_func_map.items():
            if name in KEEP_FUNCTIONS or name in post_orphans:
                continue
            body = "\n".join(output_lines[start : end + 1])
            # Only consider functions that have GUI tokens
            if not function_body_has_gui_tokens(body):
                continue
            if not has_external_ref(name, output_lines, post_func_map, post_orphans):
                post_orphans.add(name)
                changed = True

    if post_orphans:
        to_remove.update(post_orphans)
        cleaned = []
        i = 0
        while i < len(output_lines):
            name, is_static = get_function_name(output_lines[i])
            if (
                name
                and name in post_orphans
                and not is_forward_declaration(output_lines[i])
            ):
                end = find_brace_block_end(output_lines, i)
                if end is not None:
                    i = end + 1
                    continue
            cleaned.append(output_lines[i])
            i += 1
        output_lines = remove_forward_declarations(cleaned, post_orphans)

    # --- Pass 6: Cleanup stray gui_data lines ---
    clean_lines = []
    for line in output_lines:
        stripped = line.strip()
        if re.match(r"(?:const\s+)?dt_iop_\w+_gui_data_t\s*\*", stripped):
            if "self->gui_data" in stripped:
                continue
        if re.match(
            r"\(\(dt_iop_\w+_gui_data_t\s*\*\)\s*self->gui_data\)",
            stripped,
        ):
            continue
        clean_lines.append(line)
    output_lines = clean_lines

    # --- Pass 7: Collapse excessive blank lines ---
    final_lines = []
    blank_count = 0
    for line in output_lines:
        if line.strip() == "":
            blank_count += 1
            if blank_count <= 2:
                final_lines.append(line)
        else:
            blank_count = 0
            final_lines.append(line)

    with open(output_path, "w") as f:
        f.writelines(final_lines)

    print(f"Stripped: {input_path} -> {output_path}")
    print(f"  Removed functions ({len(to_remove)}): {sorted(to_remove)}")
    print(f"  Lines: {len(lines)} -> {len(final_lines)}")


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} input.c output.c", file=sys.stderr)
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = sys.argv[2]

    if not os.path.isfile(input_path):
        print(f"Error: {input_path} not found", file=sys.stderr)
        sys.exit(1)

    strip_iop(input_path, output_path)


if __name__ == "__main__":
    main()
