(module
  (type $binop (func (param i32 i32) (result i32)))
  (import "cm" "source" (func $source (type $binop)))
  (func (export "call-source-const") (result i32)
    i32.const 8
    i32.const 34
    call $source))
