(component
  (core module
    (memory (export "memory") 1)
    (global $heap (mut i32) (i32.const 256))
    (data (i32.const 128) "hello")
    (data (i32.const 144) "hello-from-guest!")
    (data (i32.const 176) "\ff\61")
    (data (i32.const 192) "\10\20\30\40")
    (data (i32.const 208) "hybrid")
    (data (i32.const 224) "\aa\bb\cc")
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
    (func $nested-list-core (export "nested-list-core") (result i32)
      (local $dst i32)
      i32.const 0
      i32.const 0
      i32.const 1
      i32.const 4
      call $cabi_realloc
      local.set $dst
      local.get $dst
      i32.const 192
      i32.const 4
      memory.copy
      i32.const 0
      local.get $dst
      i32.store
      i32.const 4
      i32.const 4
      i32.store
      i32.const 0)
    (func $mixed-list-core (export "mixed-list-core") (result i32)
      (local $msg i32)
      (local $payload i32)
      i32.const 0
      i32.const 0
      i32.const 1
      i32.const 6
      call $cabi_realloc
      local.set $msg
      local.get $msg
      i32.const 208
      i32.const 6
      memory.copy
      i32.const 0
      i32.const 0
      i32.const 1
      i32.const 3
      call $cabi_realloc
      local.set $payload
      local.get $payload
      i32.const 224
      i32.const 3
      memory.copy
      i32.const 0
      i32.const 7
      i32.store
      i32.const 4
      local.get $msg
      i32.store
      i32.const 8
      i32.const 6
      i32.store
      i32.const 12
      local.get $payload
      i32.store
      i32.const 16
      i32.const 3
      i32.store
      i32.const 0)
    (func $malformed-list-core (export "malformed-list-core") (result i32)
      i32.const 0
      i32.const -2
      i32.store
      i32.const 4
      i32.const 4
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
      i32.store)
    (func $cabi_post_nested_list (export "cabi_post_nested_list") (param $retptr i32)
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
    (func $cabi_post_malformed_list (export "cabi_post_malformed_list") (param $retptr i32)
      local.get $retptr
      i64.const 0
      i64.store)
    (func $cabi_post_mixed_list (export "cabi_post_mixed_list") (param $retptr i32)
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
      i32.load offset=12
      local.set $ptr
      local.get $retptr
      i32.load offset=16
      local.set $len
      local.get $ptr
      i32.const 0
      local.get $len
      memory.fill
      local.get $retptr
      i64.const 0
      i64.store
      local.get $retptr
      i64.const 0
      i64.store offset=8))
  (core instance $i (instantiate 0))
  (alias core export $i "memory" (core memory $mem))
  (alias core export $i "cabi_realloc" (core func $realloc-core))
  (alias core export $i "nested-core" (core func $nested-core))
  (alias core export $i "mixed-core" (core func $mixed-core))
  (alias core export $i "invalid-core" (core func $invalid-core))
  (alias core export $i "nested-list-core" (core func $nested-list-core))
  (alias core export $i "mixed-list-core" (core func $mixed-list-core))
  (alias core export $i "malformed-list-core" (core func $malformed-list-core))
  (alias core export $i "cabi_post_nested" (core func $post-nested))
  (alias core export $i "cabi_post_mixed" (core func $post-mixed))
  (alias core export $i "cabi_post_nested_list" (core func $post-nested-list))
  (alias core export $i "cabi_post_malformed_list" (core func $post-malformed-list))
  (alias core export $i "cabi_post_mixed_list" (core func $post-mixed-list))
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
  (type $nested-list-record (record (field "payload" (list u8))))
  (type $nested-list-func (func (result $nested-list-record)))
  (func $nested-list-result (type $nested-list-func)
    (canon lift (core func $nested-list-core) (memory $mem) (realloc $realloc-core)
      (post-return $post-nested-list)))
  (type $mixed-list-record
    (record (field "code" s32) (field "msg" string) (field "payload" (list u8))))
  (type $mixed-list-func (func (result $mixed-list-record)))
  (func $mixed-list-result (type $mixed-list-func)
    (canon lift (core func $mixed-list-core) (memory $mem) (realloc $realloc-core)
      string-encoding=utf8 (post-return $post-mixed-list)))
  (type $malformed-list-func (func (result $nested-list-record)))
  (func $malformed-list-result (type $malformed-list-func)
    (canon lift (core func $malformed-list-core) (memory $mem) (realloc $realloc-core)
      (post-return $post-malformed-list)))
  (export "nested-result" (func $nested-result))
  (export "mixed-result" (func $mixed-result))
  (export "invalid-result" (func $invalid-result))
  (export "nested-list-result" (func $nested-list-result))
  (export "mixed-list-result" (func $mixed-list-result))
  (export "malformed-list-result" (func $malformed-list-result)))
