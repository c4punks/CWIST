# Tutorial 17: Dynamic CSS Style Composer

Generate and compose stylesheets programmatically with the CWIST CSS composer engine.

## Key Concepts
- Initializing a `cwist_css_config` struct with `cwist_css_config_init()` and
  setting fields such as `primary_color`, `secondary_color`, and `is_dark_mode`.
- Generating a complete stylesheet string with `cwist_css_generate_stylesheet()`;
  also available as separate `cwist_css_generate_variables()` (CSS custom
  properties only) and `cwist_css_generate_utility_classes()` (utility classes
  only).

## Build and Run

```bash
mkdir build && cd build
cmake ..
make
./tut17
```
