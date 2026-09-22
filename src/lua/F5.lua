-- F5 snapshot: display the player's coordinates on screen.                                                                                         
--                                                                                                                                                  
-- Pressing F5 (host keyboard) runs this file in the in-game Lua state. It                                                                          
-- grabs the hero entity, reads its position (a CVector3), and shows the values                                                                     
-- in a single message box. The CVector3's tostring is "CVector3(x,y, z)"; its                                                                      
-- numeric fields are not reachable via pos.x, so we parse the tostring.                                                                            
                                                                                                                                                    
local function f5_get_hero()                                                                                                                        
  if QuestManager and QuestManager.HeroEntity then return QuestManager.HeroEntity end                                                               
  if Debug and Debug.GetHero then return Debug.GetHero() end                                                                                        
  if GetPlayerHero then return GetPlayerHero() end                                                                                                  
  return nil                                                                                                                                        
end                                                                                                                                                 
                                                                                                                                                    
local function f5_parse_vec3(s)                                                                                                                     
  local inner = s:match("^CVector3%s*%((.*)%)%s*$") or s                                                                                            
  local a, b, c = inner:match("([%-%d%s%.%e]+),[%s]*([%-%d%s%.%e]+),[%s]*([%-%d%s%.%e]+)")                                                          
  return tonumber(a), tonumber(b), tonumber(c)                                                                                                      
end                                                                                                                                                 
                                                                                                                                                    
local function f5_snapshot()                                                                                                                        
  local hero = f5_get_hero()                                                                                                                        
  if not hero then                                                                                                                                  
    GUI.DisplayMessageBox("F5: no hero entity found")                                                                                               
    return                                                                                                                                          
  end                                                                                                                                               
                                                                                                                                                    
  local pos = hero:GetPosition()                                                                                                                    
  local s = tostring(pos)                                                                                                                           
  local x, y, z = f5_parse_vec3(s)                                                                                                                  
                                                                                                                                                    
  local lines = { "F5 snapshot" }                                                                                                                   
  if x and y and z then                                                                                                                             
    table.insert(lines, string.format("X: %.6f", x))                                                                                                
    table.insert(lines, string.format("Y: %.6f", y))                                                                                                
    table.insert(lines, string.format("Z: %.6f", z))                                                                                                
  else                                                                                                                                              
    table.insert(lines, "parse failed: " .. s)                                                                                                      
  end                                                                                                                                               
                                                                                                                                                    
  GUI.DisplayMessageBox(table.concat(lines, "\n"))                                                                                                  
end                                                                                                                                                 
                                                                                                                                                    
local ok, err = pcall(f5_snapshot)                                                                                                                  
if not ok then                                                                                                                                      
  pcall(GUI.DisplayMessageBox, "F5 error: " .. tostring(err))                                                                                       
end