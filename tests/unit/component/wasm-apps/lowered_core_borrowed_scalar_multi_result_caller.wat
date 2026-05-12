(module
  (import "cm" "source" (func $source (param i32) (result i32 i32)))
  (import "cm" "resource-new" (func $resource_new (param i32) (result i32)))
  (import "cm" "resource-rep" (func $resource_rep (param i32) (result i32)))
  (import "cm" "resource-drop" (func $resource_drop (param i32)))

  (func (export "call-source-sum-drop") (result i32)
    (local i32 i32 i32)
    i32.const 42
    call $resource_new
    local.tee 0
    call $source
    local.set 2
    local.set 1
    local.get 1
    call $resource_rep
    local.get 2
    i32.add
    local.set 2
    local.get 1
    call $resource_drop
    local.get 0
    call $resource_drop
    local.get 2))
