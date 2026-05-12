(module
  (import "cm" "source" (func $source (param i32)))
  (import "cm" "resource-drop" (func $resource_drop (param i32)))
  (memory (export "memory") 1)

  (func (export "call-source-scalar-drop") (result i32)
    i32.const 0
    call $source
    i32.const 0
    i32.load
    call $resource_drop
    i32.const 4
    i32.load))
