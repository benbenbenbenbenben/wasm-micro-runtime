(module
  (import "cm" "source" (func $source (param i32 i32 i32 i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "abcde")
  (data (i32.const 8)
    "\20\00\00\00\01\00\00\00"
    "\21\00\00\00\01\00\00\00"
    "\22\00\00\00\03\00\00\00"
    "xyhey")
  (func (export "call-source-const") (result i32)
    i32.const 37
    i32.const 0
    i32.const 5
    i32.const 8
    i32.const 3
    call $source))
