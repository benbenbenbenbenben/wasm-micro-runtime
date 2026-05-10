(module
  (import "cm" "source" (func $source (param i32)))
  (memory (export "memory") 1)
  (func (export "call-source-len") (result i32)
    i32.const 16
    call $source
    i32.const 20
    i32.load))
