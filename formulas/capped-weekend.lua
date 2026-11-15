-- adjust_rate/v1: never add more than 10%, even if a larger premium is supplied.
return function(reference_minor, weekend, premium_bp)
  if not weekend then return reference_minor end
  local bounded_bp = premium_bp
  if bounded_bp > 1000 then bounded_bp = 1000 end
  return reference_minor + (reference_minor * bounded_bp + 5000) // 10000
end
