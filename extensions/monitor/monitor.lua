-- Host grants select the store and authority. This script cannot change prices.
local delivered = 0
return {
  api = "shylock.extension/v1",
  on_load = function()
    host.log("decision monitor loaded")
  end,
  on_event = function(event, payload_json)
    delivered = delivered + 1
    local id = host.get("$event_id")
    local checkpoint = "seen/v1/" .. id
    if host.kv_get(checkpoint) then
      host.log("duplicate " .. event .. " session=" .. delivered)
      return true
    end
    local processed = tonumber(host.kv_get("processed/v1") or "0") + 1
    local errors = tonumber(host.kv_get("formula-errors/v1") or "0")
    if event == "decision.created" and host.get("status") == "formula_error" then
      errors = errors + 1
    end
    host.kv_set("processed/v1", tostring(processed))
    host.kv_set("formula-errors/v1", tostring(errors))
    host.kv_set("last-event/v1", event)
    host.kv_set(checkpoint, "yes")
    host.log("processed=" .. processed .. " formula_errors=" .. errors .. " session=" .. delivered)
  end,
  on_unload = function()
    host.log("decision monitor unloaded")
  end
}
