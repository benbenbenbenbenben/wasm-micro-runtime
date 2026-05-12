(module
  (memory (export "memory") 1)

  (data (i32.const 32) "hello")
  (data (i32.const 40)
    "\04\00\00\00"
    "\05\00\00\00"
    "\06\00\00\00")

  (func (export "pair") (result i32)
    i32.const 0
    i32.const -17
    i32.store
    i32.const 4
    i32.const 32
    i32.store
    i32.const 8
    i32.const 5
    i32.store
    i32.const 12
    i32.const 40
    i32.store
    i32.const 16
    i32.const 3
    i32.store
    i32.const 20
    i32.const 9
    i32.store
    i32.const 0)

  (func (export "cabi_post_pair") (param i32)
    local.get 0
    i64.const 0
    i64.store
    local.get 0
    i32.const 8
    i32.add
    i64.const 0
    i64.store
    local.get 0
    i32.const 16
    i32.add
    i64.const 0
    i64.store))
