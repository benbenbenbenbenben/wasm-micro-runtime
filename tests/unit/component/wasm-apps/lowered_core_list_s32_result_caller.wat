(module
  (import "cm" "source" (func $source (param i32 i32 i32)))
  (memory (export "memory") 1)
  (data (i32.const 0) "\11\00\00\00\2a\00\00\00")
  (func (export "call-source-len") (result i32)
    i32.const 0
    i32.const 2
    i32.const 16
    call $source
    i32.const 20
    i32.load)
)
