-- Explicit no-op adjust_rate/v1 formula for forecast-only pricing policies.
return function(reference_minor, weekend, premium_bp)
  return reference_minor
end
