(module
  (import "cm" "source" (func $source (param i32)))
  (memory (export "memory") 1)
  (func (export "call-source-sum") (result i32)
    i32.const 16
    call $source
    i32.const 16
    i32.load
    i32.const 24
    i32.load
    i32.add
    i32.const 32
    i32.load
    i32.add)
  (func (export "call-source-oob") (result i32)
    i32.const 65532
    call $source
    i32.const 0))
