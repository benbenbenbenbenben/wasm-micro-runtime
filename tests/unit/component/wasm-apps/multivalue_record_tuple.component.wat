(component
  (core module
    (func $tuple-core (export "tuple-core") (result i32 i64 f32)
      i32.const 42
      i64.const 7
      f32.const 1.5)
    (func $record-core (export "record-core") (result i32 i64 f64)
      i32.const -17
      i64.const -1
      f64.const 6.25)
    (func $invalid-bool-record-core (export "invalid-bool-record-core") (result i32 i32)
      i32.const 2
      i32.const 42))
  (core instance $i (instantiate 0))
  (alias core export $i "tuple-core" (core func $tuple-core))
  (alias core export $i "record-core" (core func $record-core))
  (alias core export $i "invalid-bool-record-core" (core func $invalid-bool-record-core))
  (type $tuple-value (tuple s32 u64 f32))
  (type $tuple-func (func (result $tuple-value)))
  (func $tuple-result (type $tuple-func) (canon lift (core func $tuple-core)))
  (type $record-inner (tuple u64 f64))
  (type $record-value (record (field "lhs" s32) (field "rhs" $record-inner)))
  (type $record-func (func (result $record-value)))
  (func $record-result (type $record-func) (canon lift (core func $record-core)))
  (type $bool-record-value (record (field "ok" bool) (field "sum" s32)))
  (type $bool-record-func (func (result $bool-record-value)))
  (func $invalid-bool-result (type $bool-record-func)
    (canon lift (core func $invalid-bool-record-core)))
  (export "tuple-result" (func $tuple-result))
  (export "record-result" (func $record-result))
  (export "invalid-bool-result" (func $invalid-bool-result)))
