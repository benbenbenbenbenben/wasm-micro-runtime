(component
  (type $perms (flags "read" "write" "exec"))
  (type $f-type (func (param "x" $perms) (result s32)))
  (core module $m
    (func (export "f") (param i32) (result i32)
      local.get 0
    )
  )
  (core instance $i (instantiate $m))
  (func $g (type $f-type)
    (canon lift (core func $i "f"))
  )
  (export "g" (func $g))
)
