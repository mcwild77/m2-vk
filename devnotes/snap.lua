-- Periodic screenshots for the poly-tap sweep.
--
-- Saves a PNG at each emulated timestamp in SNAP_TIMES so the sweep can be verified visually:
-- a game sitting on a service / network-error screen must not be mistaken for one in attract mode,
-- and on Model 2 such a screen can itself be hundreds of textured translucent quads.
--
-- Usage:
--   ./mamemodel2 <set> -autoboot_script devnotes/snap.lua -snapname "%g/%i" -str 70 ...
-- Snapshots land in the snap directory (default ./snap/<gamename>/).

local times = { 20, 30, 40, 50, 60 }
local next_index = 1
local shots = 0

-- The subscription must outlive the script chunk, so it is held in a global.
_G.m2vk_snap_sub = emu.add_machine_frame_notifier(
		function()
			if next_index > #times then
				return
			end
			if emu.romname() == "___empty" then
				return
			end
			local m = manager.machine
			if m == nil then
				return
			end
			if m.time.seconds >= times[next_index] then
				m.video:snapshot()
				shots = shots + 1
				-- print_info does not reach stderr, so write directly.
				io.stderr:write(string.format("[snap] %s at %ds (shot %d)\n",
						emu.romname(), times[next_index], shots))
				next_index = next_index + 1
			end
		end)
