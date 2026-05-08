(component
  (core module
    (memory (export "memory") 1)
    (global $heap (mut i32) (i32.const 32))
    (func $cabi_realloc (export "cabi_realloc") (param i32 i32 i32 i32) (result i32)
      (local $ptr i32)
      global.get $heap
      local.set $ptr
      global.get $heap
      local.get 3
      i32.add
      global.set $heap
      local.get $ptr)
    (func $echo (export "echo") (param $ptr i32) (param $len i32) (result i32)
      (local $dst i32)
      i32.const 0
      i32.const 0
      i32.const 1
      local.get $len
      call $cabi_realloc
      local.set $dst
      local.get $dst
      local.get $ptr
      local.get $len
      memory.copy
      i32.const 0
      local.get $dst
      i32.store
      i32.const 4
      local.get $len
      i32.store
      i32.const 0)
    (func $cabi_post_echo (export "cabi_post_echo") (param $retptr i32)
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
  )
  (core instance $i (instantiate 0))
  (alias core export $i "memory" (core memory $mem))
  (alias core export $i "echo" (core func $echo-core))
  (alias core export $i "cabi_realloc" (core func $realloc-core))
  (alias core export $i "cabi_post_echo" (core func $post-core))
  (type $echo-type (func (param "name" string) (result string)))
  (func $echo-lift (type $echo-type)
    (canon lift (core func $echo-core) (memory $mem) (realloc $realloc-core) string-encoding=utf8 (post-return $post-core)))
  (export "echo" (func $echo-lift))
)
