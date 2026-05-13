(module
  (memory i64 1)
  (global $heap i64 (i64.const 64))
  (func $cabi_realloc (export "cabi_realloc") (param i64 i64 i64 i64) (result i64)
    (local $ptr i64)
    local.get 3
    i64.eqz
    if (result i64)
      i64.const 0
    else
      global.get $heap
      local.set $ptr
      global.get $heap
      local.get 3
      i64.add
      global.set $heap
      local.get $ptr
    end)
  (data (i32.const 0) "abc")
  (func (export "call-source") (param i64 i64) (result i64)
    local.get 0
    local.get 1
    call $cabi_realloc)
  (func (export "get-mem-length") (result i32)
    i32.const 3))
