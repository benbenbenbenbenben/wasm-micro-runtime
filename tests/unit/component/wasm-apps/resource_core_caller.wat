(module
  (import "host" "resource-new" (func $resource_new (param i32) (result i32)))
  (import "host" "resource-rep" (func $resource_rep (param i32) (result i32)))
  (import "host" "resource-drop" (func $resource_drop (param i32)))

  (func (export "create-handle-const") (result i32)
    i32.const 42
    call $resource_new)

  (func (export "roundtrip-rep-const") (result i32)
    (local i32)
    i32.const 42
    call $resource_new
    local.tee 0
    call $resource_rep
    local.get 0
    call $resource_drop)

  (func (export "rep-after-drop-const") (result i32)
    (local i32)
    i32.const 42
    call $resource_new
    local.tee 0
    call $resource_drop
    local.get 0
    call $resource_rep))
