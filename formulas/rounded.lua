return function(reference_minor, weekend, premium_bp)
  local rate = reference_minor
  if weekend then rate = rate + math.floor(rate * premium_bp / 10000) end
  return math.tointeger(math.max(0, rate))
end
