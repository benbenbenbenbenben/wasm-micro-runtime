(component
  (type $opt-u32 (option u32))
  (type $f-type (func (param "x" $opt-u32) (result s32)))
  (core module $m
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
    (func (export "f") (param i32 i32) (result i32)
      local.get 0
    )
  )
  (core instance $i (instantiate $m))
  (alias core export $i "memory" (core memory $mem))
  (alias core export $i "cabi_realloc" (core func $realloc))
  (func $g (type $f-type)
    (canon lift (core func $i "f") (memory $mem) (realloc $realloc))
  )
  (export "g" (func $g))
)
