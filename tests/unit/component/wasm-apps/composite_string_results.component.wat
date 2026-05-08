(component
  (core module
    (memory (export "memory") 1)
    (global $heap (mut i32) (i32.const 256))
    (data (i32.const 128) "hello")
    (data (i32.const 144) "hello-from-guest!")
    (data (i32.const 176) "\ff\61")
    (func $cabi_realloc (export "cabi_realloc") (param i32 i32 i32 i32) (result i32)
      (local $ptr i32)
      global.get $heap
      local.set $ptr
      global.get $heap
      local.get 3
      i32.add
      global.set $heap
      local.get $ptr)
    (func $nested-core (export "nested-core") (result i32)
      (local $dst i32)
      i32.const 0
      i32.const 0
      i32.const 1
      i32.const 5
      call $cabi_realloc
      local.set $dst
      local.get $dst
      i32.const 128
      i32.const 5
      memory.copy
      i32.const 0
      local.get $dst
      i32.store
      i32.const 4
      i32.const 5
      i32.store
      i32.const 0)
    (func $mixed-core (export "mixed-core") (result i32)
      (local $dst i32)
      i32.const 0
      i32.const 0
      i32.const 1
      i32.const 17
      call $cabi_realloc
      local.set $dst
      local.get $dst
      i32.const 144
      i32.const 17
      memory.copy
      i32.const 0
      i32.const 42
      i32.store
      i32.const 4
      local.get $dst
      i32.store
      i32.const 8
      i32.const 17
      i32.store
      i32.const 0)
    (func $invalid-core (export "invalid-core") (result i32)
      (local $dst i32)
      i32.const 0
      i32.const 0
      i32.const 1
      i32.const 2
      call $cabi_realloc
      local.set $dst
      local.get $dst
      i32.const 176
      i32.const 2
      memory.copy
      i32.const 0
      local.get $dst
      i32.store
      i32.const 4
      i32.const 2
      i32.store
      i32.const 0)
    (func $cabi_post_nested (export "cabi_post_nested") (param $retptr i32)
      (local $ptr i32)
      (local $len i32)
      local.get $retptr
      i32.load
      local.set $ptr
      local.get $retptr
      i32.load offset=4
      local.set $len
      local.get $ptr
      i32.const 0
      local.get $len
      memory.fill
      local.get $retptr
      i64.const 0
      i64.store)
    (func $cabi_post_mixed (export "cabi_post_mixed") (param $retptr i32)
      (local $ptr i32)
      (local $len i32)
      local.get $retptr
      i32.load offset=4
      local.set $ptr
      local.get $retptr
      i32.load offset=8
      local.set $len
      local.get $ptr
      i32.const 0
      local.get $len
      memory.fill
      local.get $retptr
      i64.const 0
      i64.store
      local.get $retptr
      i32.const 8
      i32.add
      i32.const 0
      i32.store))
  (core instance $i (instantiate 0))
  (alias core export $i "memory" (core memory $mem))
  (alias core export $i "cabi_realloc" (core func $realloc-core))
  (alias core export $i "nested-core" (core func $nested-core))
  (alias core export $i "mixed-core" (core func $mixed-core))
  (alias core export $i "invalid-core" (core func $invalid-core))
  (alias core export $i "cabi_post_nested" (core func $post-nested))
  (alias core export $i "cabi_post_mixed" (core func $post-mixed))
  (type $nested-record (record (field "msg" string)))
  (type $nested-func (func (result $nested-record)))
  (func $nested-result (type $nested-func)
    (canon lift (core func $nested-core) (memory $mem) (realloc $realloc-core)
      string-encoding=utf8 (post-return $post-nested)))
  (type $mixed-record (record (field "code" s32) (field "msg" string)))
  (type $mixed-func (func (result $mixed-record)))
  (func $mixed-result (type $mixed-func)
    (canon lift (core func $mixed-core) (memory $mem) (realloc $realloc-core)
      string-encoding=utf8 (post-return $post-mixed)))
  (type $invalid-func (func (result $nested-record)))
  (func $invalid-result (type $invalid-func)
    (canon lift (core func $invalid-core) (memory $mem) (realloc $realloc-core)
      string-encoding=utf8 (post-return $post-nested)))
  (export "nested-result" (func $nested-result))
  (export "mixed-result" (func $mixed-result))
  (export "invalid-result" (func $invalid-result)))
