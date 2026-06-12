import urllib.request

def parse_stl_game(url):
    req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
    try:
        stream = urllib.request.urlopen(req, timeout=10)
    except Exception as e:
        print(f"Error: {e}")
        return None
        
    buffer = ""
    stl_event_started = False
    stl_event_buffer = ""
    bytes_read_since_start = 0
    
    while True:
        chunk = stream.read(512)
        if not chunk:
            break
        decoded = chunk.decode('utf-8', errors='ignore')
        
        if not stl_event_started:
            buffer += decoded
            if len(buffer) > 4096:
                buffer = buffer[-2048:]
                
            idx = buffer.find('"shortName"')
            if idx != -1:
                val_start = buffer.find('"', idx + 11)
                val_end = buffer.find('"', val_start + 1) if val_start != -1 else -1
                if val_start != -1 and val_end != -1:
                    short_name = buffer[val_start+1:val_end]
                    if 'STL' in short_name:
                        print("Found STL game shortName:", short_name)
                        stl_event_started = True
                        stl_event_buffer = buffer[idx:]
        else:
            stl_event_buffer += decoded
            bytes_read_since_start += len(decoded)
            if bytes_read_since_start > 25000: # read up to 25KB to be safe
                break
                
    if not stl_event_started:
        print("STL game not found in stream.")
        return None
        
    # Extracted details
    short_name = ""
    s_idx = stl_event_buffer.find('"shortName"')
    if s_idx != -1:
        v_start = stl_event_buffer.find('"', s_idx + 11)
        v_end = stl_event_buffer.find('"', v_start + 1)
        short_name = stl_event_buffer[v_start+1:v_end]
        
    teams = []
    scores = []
    
    # Find "competitors"
    comp_idx = stl_event_buffer.find('"competitors"')
    if comp_idx != -1:
        pos = comp_idx
        for i in range(2):
            # 1. Find the next competitor of type "team"
            type_team_idx = stl_event_buffer.find('"type":"team"', pos)
            if type_team_idx == -1:
                break
                
            # 2. Find the actual team object inside this competitor
            team_idx = stl_event_buffer.find('"team":{"', type_team_idx)
            if team_idx == -1:
                break
                
            # 3. Find the abbreviation inside this team object
            abb_idx = stl_event_buffer.find('"abbreviation"', team_idx)
            if abb_idx == -1:
                break
            v_start = stl_event_buffer.find('"', abb_idx + 14)
            v_end = stl_event_buffer.find('"', v_start + 1)
            team_abbrev = stl_event_buffer[v_start+1:v_end]
            teams.append(team_abbrev)
            
            # 4. Find score inside this competitor (which follows the team block)
            score_idx = stl_event_buffer.find('"score"', v_end)
            if score_idx != -1:
                vs = stl_event_buffer.find('"', score_idx + 7)
                ve = stl_event_buffer.find('"', vs + 1)
                scores.append(stl_event_buffer[vs+1:ve])
                pos = ve
            else:
                pos = v_end

    # Status detail
    status_detail = "Unknown"
    status_idx = stl_event_buffer.find('"status"')
    if status_idx != -1:
        detail_idx = stl_event_buffer.find('"detail"', status_idx)
        if detail_idx != -1:
            vs = stl_event_buffer.find('"', detail_idx + 8)
            ve = stl_event_buffer.find('"', vs + 1)
            status_detail = stl_event_buffer[vs+1:ve]
                
    return {
        "shortName": short_name,
        "teams": teams,
        "scores": scores,
        "status": status_detail
    }

print("=== NHL Blues (on Oct 15, 2024) ===")
res_nhl = parse_stl_game('https://site.api.espn.com/apis/site/v2/sports/hockey/nhl/scoreboard?dates=20241015')
print("Parsed:", res_nhl)
