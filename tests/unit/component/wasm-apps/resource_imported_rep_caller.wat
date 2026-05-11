(module
  (import "host" "resource-rep" (func $resource_rep (param i32) (result i32)))

  (func (export "rep-handle-const") (result i32)
    i32.const 1
    call $resource_rep))
