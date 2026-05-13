(module
  (import "cm" "source" (func $source (param i32 i32) (result i32)))
  (func (export "call-source") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    call $source))
