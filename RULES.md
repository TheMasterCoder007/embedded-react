# Documentation Rules

## Section headers

- Use this style of headers to break up sections.

```
/*----------------------------------------------------------------------------------------------------------------------
 - Functions: Public
 ---------------------------------------------------------------------------------------------------------------------*/
```

## Function Documenting

- All functions static and public should be documented and contain
    - Description
    - Parameters
    - Return value (if not void)
- Public APIs should have the description in the header
- Static Private Functions should have their description in the c file where they are implemented

```
/**
 * @brief This is a function description.
 *
 * @param[in] param1 This is a parameter description.
 * @param[in] param2 This is a parameter description.
 *
 * @return This is a return value description.
 */
void function_name(int param1, int param2) {}
```

## Code Style

- The project contains a .clang-format file to enforce code style. There is a cmake configuration called format. Run
  before commiting your changes.