(module
  (import "cm" "source" (func $source (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "abcde")
  (data (i32.const 8) "\aa\bb\cc")
  (func (export "call-source-const") (result i32)
    i32.const 37
    i32.const 0
    i32.const 5
    i32.const 8
    i32.const 3
    call $source))
