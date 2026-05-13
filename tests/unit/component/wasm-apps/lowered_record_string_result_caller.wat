(module
  (import "cm" "source" (func $source (param i32)))
  (memory (export "memory") 1)
  (func (export "call-source-string-len") (result i32)
    i32.const 16
    call $source
    i32.const 24
    i32.load))
