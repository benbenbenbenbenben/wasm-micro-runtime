(module
  (import "host" "resource-drop" (func $resource_drop (param i32)))

  (func (export "drop-handle-const") (result i32)
    i32.const 1
    call $resource_drop
    i32.const 0))
