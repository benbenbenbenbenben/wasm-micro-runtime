(module
  (import "cm" "source" (func $source (param i32 i32) (result i32)))
  (memory (export "memory") 1)
  (data (i32.const 0)
    "\10\00\00\00\02\00\00\00\12\00\00\00\03\00\00\00hihey")
  (func (export "call-source-count") (result i32)
    i32.const 0
    i32.const 2
    call $source))
