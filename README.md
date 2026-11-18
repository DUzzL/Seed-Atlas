# Seed Atlas

Seed Atlas is a fast map viewer and seed-finding tool for Minecraft.



## Download

**[Download Seed Atlas](https://github.com/DUzzL/Seed-Atlas/releases)**

Read the text there! Depending on your Operating System, you might have 
to follow the steps described there.

## Features

- Explore biomes and structures in the Overworld, Nether, and End
- Switch the map to a terrain view with Minecraft-like surface colors and hill shading
- Search large areas for biomes, structures, and slime chunks
- Show the eyes of ender in the End portals of a seed, and search for seeds with many eyes or full 12-eye portals (1.18 to 26.3)
- Amplified worlds (1.18 to 26.3): biomes, strongholds and the real terrain height, checked against vanilla servers
- Datapack worlds (1.18.2, 1.19.2, 1.19.4, 1.20.6, 1.21.1 to 1.21.11 and 26.1 to 26.3): load worldgen data packs or mod jars and see that world for your seed, computed on your computer: biomes with the packs' names and colours, the packs' structures, terrain heights, the Nether and the End; search seeds and analyse biomes and structures of such worlds
- MCP server for AI assistants (Claude Desktop, Claude Code): query worlds, analyse areas and run seed searches (see below)
- Use all available CPU threads for faster searches
- Create advanced seed searches with combined conditions and Lua filters
- Save and resume searches with session files
- Supports Minecraft releases through 26.3

## Usage

1. Download and start Seed Atlas.
2. Select a Minecraft version and enter a seed.
3. Explore the map or open the Seed Finder tab.
4. Select an area and start the search.

## MCP: Seed Atlas for AI assistants

Seed Atlas can run as an [MCP](https://modelcontextprotocol.io) server, so AI
assistants such as Claude Desktop or Claude Code can query worlds and search seeds
with it. Started as `seed-atlas --mcp`, it opens no window and speaks MCP over
standard input and output. Settings, MCP shows the exact command for your
installation and copies a ready configuration entry.

Claude Code:

    claude mcp add seed-atlas -- /path/to/seed-atlas --mcp

Claude Desktop (`claude_desktop_config.json`):

    {
      "mcpServers": {
        "seed-atlas": {
          "command": "/Applications/Seed Atlas.app/Contents/MacOS/seed-atlas",
          "args": ["--mcp"]
        }
      }
    }

Seeds are passed and returned as decimal strings, coordinates are block
coordinates. The tools:

- `server_info`: versions, world types, structure types, biomes, rights and limits
- `biome_at`, `surface_height`: the biome or terrain height at positions
- `biome_map`: the biomes of an area as a compact character grid with a legend
- `structures_in_area`, `nearest_structure`: structures with positions and details
- `spawn`, `strongholds`, `end_portals`: spawn, strongholds and the eyes of their End portals
- `biome_stats`, `structure_stats`: the Biomes and Structures analyses for one or more seeds
- `start_search`, `search_status`, `search_results`, `stop_search`: Seed Finder
  searches in the background (structure, biome, spawn and stronghold conditions, or
  any condition of the app as a `#Cond:` line)
- `load_datapacks`, `datapack_info`, `unload_datapacks`: datapack worlds from local
  packs; the returned handle works with the tools above. The game data is taken from
  where the app finds it; the server never downloads it.
- `export_search`: writes search results to a file (only with the write right)

Settings, MCP decides what clients may do: "Read and compute" (the default: queries,
analyses and searches, no files written, the app's settings never changed) or "Also
write files" (export to paths the client names). It also limits the search threads
and the duration of a search. The server reads these settings when it starts.

## License

Seed Atlas is licensed under the [GNU GPLv3](LICENSE). Attribution and bundled
component licenses are documented in [LEGAL_NOTICE.md](LEGAL_NOTICE.md) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Seed Atlas is based on cubiomes-viewer by Cubitect. 

Seed Atlas is not an official Minecraft product and is not associated with
Mojang or Microsoft.

<img width="2560" height="1392" alt="Screenshot 2026-07-17 220702" src="https://github.com/user-attachments/assets/def90d1e-630b-4441-9fb3-526a4435c78d" />
<img width="2560" height="1392" alt="Screenshot 2026-07-17 221002" src="https://github.com/user-attachments/assets/9946fa84-132f-4c52-9c92-744a1c9ec165" />
<img width="2560" height="1392" alt="Screenshot 2026-07-17 221150" src="https://github.com/user-attachments/assets/d93c183e-cfb0-4d2b-8fba-8b2a253850f0" />
