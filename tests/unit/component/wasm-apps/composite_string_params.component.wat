(component
  (core module
    (memory (export "memory") 1)
    (global $heap (mut i32) (i32.const 64))
    (func $cabi_realloc (export "cabi_realloc") (param i32 i32 i32 i32) (result i32)
      (local $ptr i32)
      local.get 3
      i32.eqz
      if (result i32)
        i32.const 0
      else
        global.get $heap
        local.set $ptr
        global.get $heap
        local.get 3
        i32.add
        global.set $heap
        local.get $ptr
      end)
    (func $nested-core (export "nested-core") (param i32 i32) (result i32)
      local.get 1)
    (func $mixed-core (export "mixed-core") (param i32 i32 i32) (result i32)
      local.get 0
      local.get 2
      i32.add)
    (func $nested-list-core (export "nested-list-core") (param i32 i32) (result i32)
      local.get 1)
    (func $mixed-list-core (export "mixed-list-core")
      (param i32 i32 i32 i32 i32) (result i32)
      local.get 0
      local.get 2
      i32.add
      local.get 4
      i32.add))
  (core instance $i (instantiate 0))
  (alias core export $i "memory" (core memory $mem))
  (alias core export $i "cabi_realloc" (core func $realloc-core))
  (alias core export $i "nested-core" (core func $nested-core))
  (alias core export $i "mixed-core" (core func $mixed-core))
   (alias core export $i "nested-list-core" (core func $nested-list-core))
   (alias core export $i "mixed-list-core" (core func $mixed-list-core))
  (type $string-tuple (tuple string))
  (type $nested-record (record (field "msg" $string-tuple)))
  (type $nested-func (func (param "input" $nested-record) (result s32)))
  (func $nested-param (type $nested-func)
    (canon lift (core func $nested-core) (memory $mem) (realloc $realloc-core)
      string-encoding=utf8))
  (type $mixed-record (record (field "code" s32) (field "payload" $string-tuple)))
  (type $mixed-func (func (param "input" $mixed-record) (result s32)))
  (func $mixed-param (type $mixed-func)
    (canon lift (core func $mixed-core) (memory $mem) (realloc $realloc-core)
      string-encoding=utf8))
  (type $list-tuple (tuple (list u8)))
  (type $nested-list-record (record (field "payload" $list-tuple)))
  (type $nested-list-func (func (param "input" $nested-list-record) (result s32)))
  (func $nested-list-param (type $nested-list-func)
    (canon lift (core func $nested-list-core) (memory $mem) (realloc $realloc-core)))
  (type $mixed-list-payload (tuple string (list u8)))
  (type $mixed-list-record
    (record (field "code" s32) (field "payload" $mixed-list-payload)))
  (type $mixed-list-func (func (param "input" $mixed-list-record) (result s32)))
  (func $mixed-list-param (type $mixed-list-func)
    (canon lift (core func $mixed-list-core) (memory $mem) (realloc $realloc-core)
      string-encoding=utf8))
  (export "nested-param" (func $nested-param))
  (export "mixed-param" (func $mixed-param))
  (export "nested-list-param" (func $nested-list-param))
  (export "mixed-list-param" (func $mixed-list-param)))
