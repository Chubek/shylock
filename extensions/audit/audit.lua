return {
  api = "shylock.extension/v1",
  on_load = function()
    host.log("audit extension loaded")
  end,
  on_event = function(event, payload_json)
    host.log(event .. " " .. host.get("$event_id"))
  end,
  on_unload = function()
    host.log("audit extension unloaded")
  end
}
