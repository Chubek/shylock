-- adjust_rate/v1: use the configured basis points as a weekday discount.
-- This is a pure calculation. On1x, not this function, applies floor/cap/tick.
return function(reference_minor, weekend, premium_bp)
  if weekend then return reference_minor end
  return reference_minor - (reference_minor * premium_bp + 5000) // 10000
end
