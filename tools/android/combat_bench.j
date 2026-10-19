// Diagnostic extension for a local copy of Battle Simulator v0.19d.
globals
    unit array combat_units
    real array combat_x
    real array combat_y
    integer array combat_types
    integer array combat_limits
    integer array combat_spells
    real array combat_mana
    timer combat_timer = null
    trigger combat_events = null
    boolean combat_props = false
    boolean combat_right = false
    integer combat_ticks = 0
    integer combat_deaths = 0
    integer combat_spends = 0
    integer combat_orders = 0
    real combat_initial_hp = 0.0
    string combat_phase = "idle"
endglobals

function Combat_Configure takes nothing returns nothing
// ROSTER
endfunction

function Combat_OwnUnit takes unit u returns boolean
    return GetUnitUserData(u) >= 9000 and GetUnitUserData(u) < 9200
endfunction

function Combat_Death takes nothing returns nothing
    if Combat_OwnUnit(GetTriggerUnit()) then
        set combat_deaths = combat_deaths + 1
    endif
endfunction

function Combat_Status takes nothing returns nothing
    local integer i = 0
    local integer alive = 0
    local integer wounded = 0
    local integer attacking = 0
    local integer moving = 0
    local real hp = 0.0
    local unit u
    loop
        exitwhen i >= 200
        set u = combat_units[i]
        if GetWidgetLife(u) > 0.405 then
            set alive = alive + 1
            set hp = hp + GetWidgetLife(u)
            if GetWidgetLife(u) < GetUnitState(u, UNIT_STATE_MAX_LIFE)-1.0 then
                set wounded = wounded + 1
            endif
            if GetUnitCurrentOrder(u) == OrderId("attack") then
                set attacking = attacking + 1
            elseif GetUnitCurrentOrder(u) == OrderId("move") then
                set moving = moving + 1
            endif
        endif
        set i = i + 1
    endloop
    call ClearTextMessages()
    call DisplayTimedTextToPlayer(Player(0), 0.01, 0.0, 90.0, "COMBAT "+combat_phase+" ticks="+I2S(combat_ticks)+" alive="+I2S(alive)+" dead="+I2S(combat_deaths)+" wounded="+I2S(wounded)+" attack="+I2S(attacking)+" move="+I2S(moving)+"|nHP="+R2S(hp)+"/"+R2S(combat_initial_hp)+" mana_costs="+I2S(combat_spends)+" orders="+I2S(combat_orders)+" blizzard="+I2S(combat_spells[0])+" knives="+I2S(combat_spells[1])+" nova="+I2S(combat_spells[2])+" storm="+I2S(combat_spells[3]))
    set u = null
endfunction

// The original map's actors and triggers remain active outside the arena.
function Combat_Props takes nothing returns nothing
    local integer i = 0
    local unit u
    if combat_props then
        return
    endif
    set combat_props = true
    loop
        exitwhen i >= 16
        call CreateDestructable('LTlt', 3400.0+I2R(i)*155.0, -5500.0, I2R(i*31), 1.0, ModuloInteger(i, 10))
        call CreateDestructable('LTlt', 3400.0+I2R(i)*155.0, -3700.0, I2R(i*31), 1.0, ModuloInteger(i, 10))
        call CreateDestructable('LTlt', 3300.0, -5400.0+I2R(i)*110.0, I2R(i*31), 1.0, ModuloInteger(i, 10))
        call CreateDestructable('LTlt', 5900.0, -5400.0+I2R(i)*110.0, I2R(i*31), 1.0, ModuloInteger(i, 10))
        set i = i + 1
    endloop
// BUILDINGS
endfunction

function Combat_Setup takes nothing returns nothing
    local integer i = 0
    local integer slot = 0
    local integer kind = 0
    local integer side = 0
    local integer spell = 0
    local real x = 0.0
    local real y = 0.0
    local unit u
    if combat_timer != null then
        call PauseTimer(combat_timer)
        call DestroyTimer(combat_timer)
        set combat_timer = null
    endif
    call Combat_Configure()
    call Combat_Props()
    call SetFloatGameState(GAME_STATE_TIME_OF_DAY, 12.0)
    call SuspendTimeOfDay(true)
    call SetPlayerAllianceStateBJ(Player(2), Player(4), bj_ALLIANCE_ALLIED)
    call SetPlayerAllianceStateBJ(Player(4), Player(2), bj_ALLIANCE_ALLIED)
    call SetPlayerAllianceStateBJ(Player(2), Player(PLAYER_NEUTRAL_PASSIVE), bj_ALLIANCE_ALLIED)
    call SetPlayerAllianceStateBJ(Player(4), Player(PLAYER_NEUTRAL_PASSIVE), bj_ALLIANCE_ALLIED)
    call SetPlayerAllianceStateBJ(Player(PLAYER_NEUTRAL_PASSIVE), Player(2), bj_ALLIANCE_ALLIED)
    call SetPlayerAllianceStateBJ(Player(PLAYER_NEUTRAL_PASSIVE), Player(4), bj_ALLIANCE_ALLIED)
    call SetPlayerAlliance(Player(2), Player(0), ALLIANCE_SHARED_VISION, true)
    call SetPlayerAlliance(Player(4), Player(0), ALLIANCE_SHARED_VISION, true)
    loop
        exitwhen i >= 200
        if combat_units[i] != null then
            call RemoveUnit(combat_units[i])
        endif
        set slot = ModuloInteger(i, 100)
        set side = i/100
        set kind = 0
        loop
            exitwhen slot < combat_limits[kind]
            set kind = kind + 1
        endloop
        // 72-unit spacing clears the 32-unit collision radii of the large troops.
        set x = 3700.0+I2R(ModuloInteger(slot,10))*72.0+I2R(side)*1152.0
        set y = -4924.0+I2R(slot/10)*72.0
        set u = CreateUnit(Player(2+side*2), combat_types[kind+side*10], x, y, I2R(side)*180.0)
        set combat_units[i] = u
        call SetUnitUserData(u, 9000+i)
        set combat_x[i] = GetUnitX(u)
        set combat_y[i] = GetUnitY(u)
        if kind >= 8 then
            call SetHeroLevel(u, 6, false)
            if side == 0 and kind == 8 then
                set spell = 'AHbz'
            elseif side == 0 then
                set spell = 'AEfk'
            elseif kind == 8 then
                set spell = 'AOww'
            else
                set spell = 'AUfn'
            endif
            call UnitAddAbility(u, spell)
        endif
        call SetUnitState(u, UNIT_STATE_LIFE, GetUnitState(u, UNIT_STATE_MAX_LIFE))
        call SetUnitState(u, UNIT_STATE_MANA, GetUnitState(u, UNIT_STATE_MAX_MANA))
        set combat_mana[i] = GetUnitState(u, UNIT_STATE_MANA)
        call IssueImmediateOrder(u, "holdposition")
        set i = i + 1
    endloop
    // Count hero-adjusted starting HP; setup never heals the running fight.
    set combat_initial_hp = 0.0
    set i = 0
    loop
        exitwhen i >= 200
        set combat_initial_hp = combat_initial_hp + GetWidgetLife(combat_units[i])
        set i = i + 1
    endloop
    if combat_events == null then
        set combat_events = CreateTrigger()
        call TriggerRegisterAnyUnitEventBJ(combat_events, EVENT_PLAYER_UNIT_DEATH)
        call TriggerAddAction(combat_events, function Combat_Death)
    endif
    set combat_deaths = 0
    set combat_spends = 0
    set combat_orders = 0
    set combat_ticks = 0
    set i = 0
    loop
        exitwhen i >= 4
        set combat_spells[i] = 0
        set i = i + 1
    endloop
    set combat_phase = "idle"
    call Combat_Status()
    set u = null
endfunction

function Combat_MoveTick takes nothing returns nothing
    local integer i = 0
    local real offset = -250.0
    set combat_right = not combat_right
    if combat_right then
        set offset = 250.0
    endif
    loop
        exitwhen i >= 200
        if i < 100 then
            call IssuePointOrder(combat_units[i], "move", combat_x[i]+offset, combat_y[i])
        else
            call IssuePointOrder(combat_units[i], "move", combat_x[i]-offset, combat_y[i])
        endif
        set i = i + 1
    endloop
endfunction

function Combat_Move takes nothing returns nothing
    call Combat_Setup()
    set combat_phase = "move"
    set combat_right = false
    set combat_timer = CreateTimer()
    call Combat_MoveTick()
    call TimerStart(combat_timer, 6.0, true, function Combat_MoveTick)
endfunction

// Scan just one team, once per nine-second spell wave, to select a reachable nearby target.
function Combat_Target takes unit caster, boolean ally returns unit
    local integer i = 0
    local integer limit = 100
    local real best = 1000000000.0
    local real dist = 0.0
    local unit u
    local unit result = null
    if (GetPlayerId(GetOwningPlayer(caster)) == 4) == ally then
        set i = 100
        set limit = 200
    endif
    loop
        exitwhen i >= limit
        set u = combat_units[i]
        if GetWidgetLife(u) > 0.405 then
            if not ally or GetWidgetLife(u) < GetUnitState(u, UNIT_STATE_MAX_LIFE)-1.0 then
                set dist = (GetUnitX(u)-GetUnitX(caster))*(GetUnitX(u)-GetUnitX(caster))+(GetUnitY(u)-GetUnitY(caster))*(GetUnitY(u)-GetUnitY(caster))
                if dist < best then
                    set best = dist
                    set result = u
                endif
            endif
        endif
        set i = i + 1
    endloop
    set u = null
    return result
endfunction

function Combat_FightTick takes nothing returns nothing
    local integer i = 0
    local integer id = 0
    local unit u
    local unit target
    local boolean ok = false
    set combat_ticks = combat_ticks + 1
    loop
        exitwhen i >= 200
        set u = combat_units[i]
        if GetWidgetLife(u) > 0.405 then
            // One-second mana-cost observations may combine several casts.
            // No mana-drain units are present; spell-event ordinals differ in old common.j.
            set id = GetUnitTypeId(u)
            if GetUnitState(u, UNIT_STATE_MANA) < combat_mana[i]-10.0 then
                set combat_spends = combat_spends + 1
                if id == 'Hamg' then
                    set combat_spells[0] = combat_spells[0] + 1
                elseif id == 'Ewar' then
                    set combat_spells[1] = combat_spells[1] + 1
                elseif id == 'Ulic' then
                    set combat_spells[2] = combat_spells[2] + 1
                elseif id == 'Obla' then
                    set combat_spells[3] = combat_spells[3] + 1
                endif
            endif
            set combat_mana[i] = GetUnitState(u, UNIT_STATE_MANA)
            if GetUnitCurrentOrder(u) == 0 then
                call IssuePointOrder(u, "attack", 4600.0, -4600.0)
            endif
            if ModuloInteger(combat_ticks, 9) == 2 then
                set ok = false
                if id == 'Hamg' or id == 'Ulic' or id == 'hmpr' or id == 'hsor' or id == 'oshm' or id == 'unec' or id == 'uban' then
                    set target = Combat_Target(u, id == 'hmpr' or id == 'oshm')
                else
                    set target = null
                endif
                if id == 'Hamg' and target != null then
                    set ok = IssuePointOrder(u, "blizzard", GetUnitX(target), GetUnitY(target))
                elseif id == 'Ewar' then
                    set ok = IssueImmediateOrder(u, "fanofknives")
                elseif id == 'Obla' then
                    set ok = IssueImmediateOrder(u, "whirlwind")
                elseif target != null then
                    if id == 'Ulic' then
                        set ok = IssueTargetOrder(u, "frostnova", target)
                    elseif id == 'hmpr' then
                        set ok = IssueTargetOrder(u, "heal", target)
                    elseif id == 'hsor' then
                        set ok = IssueTargetOrder(u, "slow", target)
                    elseif id == 'oshm' then
                        set ok = IssueTargetOrder(u, "bloodlust", target)
                    elseif id == 'unec' then
                        set ok = IssueTargetOrder(u, "cripple", target)
                    elseif id == 'uban' then
                        set ok = IssueTargetOrder(u, "curse", target)
                    endif
                endif
                if ok then
                    set combat_orders = combat_orders + 1
                endif
            endif
        endif
        set i = i + 1
    endloop
    set u = null
    set target = null
endfunction

function Combat_Fight takes nothing returns nothing
    local integer i = 0
    call Combat_Setup()
    set combat_phase = "fight"
    call SetPlayerAllianceStateBJ(Player(2), Player(4), bj_ALLIANCE_UNALLIED)
    call SetPlayerAllianceStateBJ(Player(4), Player(2), bj_ALLIANCE_UNALLIED)
    loop
        exitwhen i >= 200
        call IssuePointOrder(combat_units[i], "attack", 4600.0, -4600.0)
        set i = i + 1
    endloop
    set combat_timer = CreateTimer()
    call TimerStart(combat_timer, 1.0, true, function Combat_FightTick)
endfunction
