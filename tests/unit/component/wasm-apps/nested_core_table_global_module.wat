(module
  (table (export "table") 1 funcref)
  (global (export "global") i32 (i32.const 42))
  (memory (export "memory") 1)
  (func (export "func") (result i32)
    global.get 0
  )
)
