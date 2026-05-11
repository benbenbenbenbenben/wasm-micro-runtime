(module
  (import "host" "resource-new" (func $resource_new (param i32) (result i32)))

  (func (export "create-handle-const") (result i32)
    i32.const 42
    call $resource_new))
