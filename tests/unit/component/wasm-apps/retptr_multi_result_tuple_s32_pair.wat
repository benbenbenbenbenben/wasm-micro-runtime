(module
  (memory (export "memory") 1)

  (func (export "pair") (result i32)
    i32.const 0
    i32.const 11
    i32.store
    i32.const 4
    i32.const 31
    i32.store
    i32.const 8
    i32.const 7
    i32.store
    i32.const 0)

  (func (export "cabi_post_pair") (param i32)
    local.get 0
    i64.const 0
    i64.store
    local.get 0
    i32.const 8
    i32.add
    i32.const 0
    i32.store))
