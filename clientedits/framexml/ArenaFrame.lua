-- Interface/FrameXML/ArenaFrame.lua
-- Extend Blizzard arena battlemaster frame to support 4 rated + 4 practice
-- while keeping the stock ArenaFrame.xml.

MAX_ARENA_TEAMS   = 4      -- 2v2, 3v3, 4v4, 5v5
MAX_ARENA_BATTLES = 8      -- 4 rated + 4 practice
NO_ARENA_SEASON   = 0

-- CENTURION: Baradin Hold: Four Corners. Its battlemaster opens this same
-- window, and the server says which queue the window is for just before it
-- opens it:
--
--   CCGAME ARENAQUEUE:0   an ordinary arena battlemaster
--   CCGAME ARENAQUEUE:<n> a match of up to n teams (Four Corners)
--
-- With teams, every row still means what it always does - rated rows queue an
-- arena team of that size, practice rows a skirmish - but each is labelled for
-- n teams of that size ("2v2v2v2"), and the title and description say which
-- queue this is. The layout and the "Arena bot matches" box are untouched.
CENTURION_ARENA_SQUADS = 0
CENTURION_FOUR_CORNERS_TITLE = "Four Corners"
CENTURION_FOUR_CORNERS_TEXT = "Up to four teams, one in each corner of Baradin Hold. Every other team is your enemy, and the last team standing wins."

-- CENTURION: Buffle Shuffle. Its battlemaster sends CCGAME ARENAQUEUE:SHUFFLE.
-- The window then offers the two queues it takes - a 2v2 or a 3v3 skirmish,
-- alone or with a party of up to four or six, who all land in the same match
-- and are dealt into the teams like anyone else - on the practice line beside
-- the "Arena bot matches" box, which a shuffle obeys like any skirmish. The
-- rated rows are put away (there is no rated shuffle) and the rated header
-- says what the mode is instead. A party of six is a raid, so BattlemasterList
-- row 6 carries MaxGroupSize 6: the client checks the group against the row of
-- the list it shows, and the server sends this one as All Arenas.
CENTURION_ARENA_SHUFFLE = false
CENTURION_SOLO_SHUFFLE_TITLE = "Buffle Shuffle"
CENTURION_SOLO_SHUFFLE_HEADER = "Solo or grouped. New teams every round."
CENTURION_SOLO_SHUFFLE_ROWS = {
    [MAX_ARENA_TEAMS + 1] = "2v2 Buffle Shuffle",
    [MAX_ARENA_TEAMS + 2] = "3v3 Buffle Shuffle",
}
CENTURION_SOLO_SHUFFLE_TEXT = "The teams are dealt again before every round: six rounds of 3v3, or three of 2v2. Win more rounds than you lose."
-- Picked when the window opens on anything else: the 3v3 row.
local SHUFFLE_DEFAULT_ROW = MAX_ARENA_TEAMS + 2

-- CENTURION: rated arenas played like skirmishes. Sent with every arena window,
-- just after ARENAQUEUE:
--
--   CCGAME ARENARATED:1   the realm fills rated arenas with bots: a rated row
--                         can be joined alone, or by a party smaller than the
--                         arena, and the server makes up the rest (and makes a
--                         character queueing alone a team, if it has none)
--   CCGAME ARENARATED:0   stock: a rated row is a whole arena team's to join
--
-- A server built before this sends nothing, which reads as 0.
CENTURION_ARENA_RATED_FILL = false
CENTURION_ARENA_RATED_FILL_TEXT = "Rated: queue alone or with part of your team. If no real team turns up, bots fill the empty seats, and the result still counts for your rating."

-- Whether the rated rows can be joined in the window as it is now.
local function ArenaRatedJoinable()
    return CENTURION_ARENA_RATED_FILL and CENTURION_ARENA_SQUADS == 0 and not CENTURION_ARENA_SHUFFLE
        and GetCurrentArenaSeason() ~= NO_ARENA_SEASON
end

-- "2v2" for n = 2, "2v2v2v2" for four teams of two.
local function TeamsLabel(size, teams)
    local label = tostring(size)
    for n = 2, teams do
        label = label .. "v" .. size
    end
    return label
end

-- Blizzard's top list position
local RATED_X     = 23
local RATED_Y_TOP = -79

local function ArenaFrame_EnsureButtons()
    -- make sure the additional buttons exist

    -- 4th rated (5v5)
    if not _G.ArenaZone4 then
        local btn = CreateFrame("Button", "ArenaZone4", ArenaFrame, "ArenaButtonTemplate")
        btn:SetID(4)
        btn:SetScript("OnClick", ArenaButton_OnClick)
    end

    -- extra practice rows
    if not _G.ArenaZone7 then
        local btn = CreateFrame("Button", "ArenaZone7", ArenaFrame, "ArenaButtonTemplate")
        btn:SetID(7)
        btn:SetScript("OnClick", ArenaButton_OnClick)
    end
    if not _G.ArenaZone8 then
        local btn = CreateFrame("Button", "ArenaZone8", ArenaFrame, "ArenaButtonTemplate")
        btn:SetID(8)
        btn:SetScript("OnClick", ArenaButton_OnClick)
    end
end

local function ArenaFrame_Position()
    ArenaFrame_EnsureButtons()

    ----------------------------------------------------------------
    -- 1) Rated rows – force exact positions (top parchment)
    ----------------------------------------------------------------
    if ArenaZone1 then
        ArenaZone1:ClearAllPoints()
        ArenaZone1:SetPoint("TOPLEFT", ArenaFrame, "TOPLEFT", RATED_X, RATED_Y_TOP)
    end
    if ArenaZone2 then
        ArenaZone2:ClearAllPoints()
        ArenaZone2:SetPoint("TOPLEFT", ArenaZone1, "BOTTOMLEFT", 0, 0)
    end
    if ArenaZone3 then
        ArenaZone3:ClearAllPoints()
        ArenaZone3:SetPoint("TOPLEFT", ArenaZone2, "BOTTOMLEFT", 0, 0)
    end
    if ArenaZone4 then
        -- new 5v5 rated row
        ArenaZone4:ClearAllPoints()
        ArenaZone4:SetPoint("TOPLEFT", ArenaZone3, "BOTTOMLEFT", 0, 0)
    end

    ----------------------------------------------------------------
    -- 2) Practice rows – line up horizontally with rated, but sit
    --    under the "Practice Battle:" header. Drop a bit so they
    --    don't clip the grey border.
    ----------------------------------------------------------------
    if ArenaFrameNameHeader2 then
        -- row 5
        if ArenaZone5 then
            ArenaZone5:ClearAllPoints()
            -- horizontal: same as rated block
            ArenaZone5:SetPoint("LEFT", ArenaFrame, "LEFT", RATED_X, 0)
            -- vertical: under the header, but with extra gap (-8)
            ArenaZone5:SetPoint("TOP", ArenaFrameNameHeader2, "BOTTOM", 0, -10)
        end
        -- row 6
        if ArenaZone6 then
            ArenaZone6:ClearAllPoints()
            ArenaZone6:SetPoint("LEFT", ArenaFrame, "LEFT", RATED_X, 0)
            ArenaZone6:SetPoint("TOP", ArenaZone5, "BOTTOM", 0, 0)
        end
        -- row 7
        if ArenaZone7 then
            ArenaZone7:ClearAllPoints()
            ArenaZone7:SetPoint("LEFT", ArenaFrame, "LEFT", RATED_X, 0)
            ArenaZone7:SetPoint("TOP", ArenaZone6, "BOTTOM", 0, 0)
        end
        -- row 8
        if ArenaZone8 then
            ArenaZone8:ClearAllPoints()
            ArenaZone8:SetPoint("LEFT", ArenaFrame, "LEFT", RATED_X, 0)
            ArenaZone8:SetPoint("TOP", ArenaZone7, "BOTTOM", 0, 0)
        end
    end

    ----------------------------------------------------------------
    -- 3) Flavor text – in orange box, same left edge as lists
    ----------------------------------------------------------------
    if ArenaFrameZoneDescription and ArenaZone8 then
        ArenaFrameZoneDescription:ClearAllPoints()
        -- match left = 23, drop enough to be clearly in orange
        ArenaFrameZoneDescription:SetPoint("LEFT", ArenaFrame, "LEFT", RATED_X, 0)
        ArenaFrameZoneDescription:SetPoint("TOP", ArenaZone8, "BOTTOM", 0, -40)
        ArenaFrameZoneDescription:SetWidth(294)
        ArenaFrameZoneDescription:SetHeight(40)
    end
end

function ArenaFrame_OnLoad(self)
    self:RegisterEvent("BATTLEFIELDS_SHOW")
    self:RegisterEvent("BATTLEFIELDS_CLOSED")
    self:RegisterEvent("UPDATE_BATTLEFIELD_STATUS")
    self:RegisterEvent("PARTY_LEADER_CHANGED")

    ArenaFrame_Position()
end

function ArenaFrame_OnEvent(self, event, ...)
    if not IsBattlefieldArena() then
        return
    end

    if event == "BATTLEFIELDS_SHOW" then
        ShowUIPanel(ArenaFrame)

        -- make sure widgets are laid out
        ArenaFrame_Position()

        -- default selection (a shuffle has no rated rows: ArenaFrame_Update
        -- picks its own default, the 3v3 that takes any party)
        if ((GetNumPartyMembers() > 0) or (GetNumRaidMembers() > 0))
            and IsPartyLeader()
            and GetCurrentArenaSeason() ~= NO_ARENA_SEASON
            and not CENTURION_ARENA_SHUFFLE
        then
            -- parties go to practice
            if not ArenaFrame.selection or ArenaFrame.selection < (MAX_ARENA_TEAMS + 1) then
                ArenaFrame.selection = MAX_ARENA_TEAMS + 1
            end
        else
            -- solo → 2v2 rated
            ArenaFrame.selection = ArenaFrame.selection or 1
        end

        -- standard text
        if ArenaFrameZoneDescription then
            if CENTURION_ARENA_SHUFFLE then
                ArenaFrameZoneDescription:SetText(CENTURION_SOLO_SHUFFLE_TEXT)
            elseif CENTURION_ARENA_SQUADS > 0 then
                ArenaFrameZoneDescription:SetText(CENTURION_FOUR_CORNERS_TEXT)
            elseif GetCurrentArenaSeason() == NO_ARENA_SEASON then
                ArenaFrameZoneDescription:SetText(ARENA_MASTER_NO_SEASON_TEXT)
            else
                ArenaFrameZoneDescription:SetText(ARENA_MASTER_TEXT)
            end
        end

        ArenaFrame_Update()

    elseif event == "BATTLEFIELDS_CLOSED" then
        HideUIPanel(ArenaFrame)
    else
        ArenaFrame_Update()
    end
end

function ArenaButton_OnClick(self)
    ArenaFrame.selection = self:GetID()
    ArenaFrame_Update()
end

function ArenaFrame_Update()
    -- 4 rated: 2s, 3s, 4s, 5s
    local ratedSizes = {
        [1] = 2,
        [2] = 3,
        [3] = 4,
        [4] = 5,
    }

    -- 4 practice: 2s, 3s, 4s, 5s skirmish
    local practiceLabels = {
        [1] = "2v2 Skirmish",
        [2] = "3v3 Skirmish",
        [3] = "4v4 Skirmish",
        [4] = "5v5 Skirmish",
    }

    -- Four Corners: n teams of each size.
    local squads = CENTURION_ARENA_SQUADS
    if squads > 0 then
        for idx = 1, MAX_ARENA_TEAMS do
            practiceLabels[idx] = TeamsLabel(ratedSizes[idx], squads) .. " Skirmish"
        end
    end
    local shuffle = CENTURION_ARENA_SHUFFLE
    if shuffle then
        for row, label in pairs(CENTURION_SOLO_SHUFFLE_ROWS) do
            practiceLabels[row - MAX_ARENA_TEAMS] = label
        end
        if not CENTURION_SOLO_SHUFFLE_ROWS[ArenaFrame.selection or 0] then
            ArenaFrame.selection = SHUFFLE_DEFAULT_ROW
        end
    end
    if ArenaFrameFrameLabel then
        if shuffle then
            ArenaFrameFrameLabel:SetText(CENTURION_SOLO_SHUFFLE_TITLE)
        else
            ArenaFrameFrameLabel:SetText(squads > 0 and CENTURION_FOUR_CORNERS_TITLE or ARENA_BATTLES)
        end
    end
    if ArenaFrameNameHeader then
        ArenaFrameNameHeader:SetText(shuffle and CENTURION_SOLO_SHUFFLE_HEADER or ARENA_RATED_BATTLE)
    end

    for i = 1, MAX_ARENA_BATTLES do
        local button = _G["ArenaZone"..i]
        if button and shuffle and not CENTURION_SOLO_SHUFFLE_ROWS[i] then
            button:Hide()
        elseif button then
            button:Show()
            if i <= MAX_ARENA_TEAMS then
                local size = ratedSizes[i]
                if squads > 0 then
                    button:SetText(TeamsLabel(size, squads) .. " " .. ARENA_RATED)
                else
                    button:SetText(string.format(PVP_TEAMTYPE, size, size) .. " " .. ARENA_RATED)
                end
                if GetCurrentArenaSeason() == NO_ARENA_SEASON then
                    button:Disable()
                else
                    button:Enable()
                end
            else
                local idx = i - MAX_ARENA_TEAMS
                button:SetText(practiceLabels[idx] or "Skirmish")
                button:Enable()
            end

            if ArenaFrame.selection == i then
                button:LockHighlight()
            else
                button:UnlockHighlight()
            end
        end
    end

    -- Practice queues are joinable from this frame (a shuffle's rows are all
    -- practice rows, alone or as a group), and the rated ones too where the
    -- realm fills rated arenas with bots - alone, or by a party in its leader's
    -- arena team.
    local ratedRow = ArenaFrame.selection and ArenaFrame.selection <= MAX_ARENA_TEAMS
    if ArenaFrame.selection and (not ratedRow or ArenaRatedJoinable()) then
        ArenaFrameJoinButton:Enable()
        ArenaFrameGroupJoinButton:Show()
        if (((GetNumPartyMembers() > 0) or (GetNumRaidMembers() > 0)) and IsPartyLeader()) then
            ArenaFrameGroupJoinButton:Enable()
        else
            ArenaFrameGroupJoinButton:Disable()
        end
    else
        ArenaFrameJoinButton:Disable()
        ArenaFrameGroupJoinButton:Hide()
    end

    -- what the selected row is, in the ordinary window
    if ArenaFrameZoneDescription and not shuffle and squads == 0 and GetCurrentArenaSeason() ~= NO_ARENA_SEASON then
        if ratedRow and ArenaRatedJoinable() then
            ArenaFrameZoneDescription:SetText(CENTURION_ARENA_RATED_FILL_TEXT)
        else
            ArenaFrameZoneDescription:SetText(ARENA_MASTER_TEXT)
        end
    end
end

function ArenaFrameJoinButton_OnClick(self)
    local GROUPJOIN_BUTTONID = 2

    if ArenaFrame.selection and ArenaFrame.selection <= MAX_ARENA_TEAMS then
        -- rated 1..4. "Join" in a party means alone; out of one it sends what
        -- stock always sent for rated, which the server reads as alone when
        -- there is no group to queue (CCGAME ARENARATED).
        local inGroup = (GetNumPartyMembers() > 0) or (GetNumRaidMembers() > 0)
        if self:GetID() ~= GROUPJOIN_BUTTONID and inGroup and CENTURION_ARENA_RATED_FILL then
            JoinBattlefield(ArenaFrame.selection, nil, 1)
        else
            JoinBattlefield(ArenaFrame.selection, 1, 1)
        end
    elseif ArenaFrame.selection then
        -- practice 5..8 → 1..4
        local idx = ArenaFrame.selection - MAX_ARENA_TEAMS
        if self:GetID() == GROUPJOIN_BUTTONID then
            JoinBattlefield(idx, 1)
        else
            JoinBattlefield(idx)
        end
    end

    HideUIPanel(ArenaFrame)
end

-- CENTURION: "Arena bot matches" - whether an arena queue this character is
-- waiting in may be popped as a clone-filled match: a skirmish, and a rated
-- arena where the realm fills those too (CCGAME ARENARATED).
--
--   CCGAMEREQ ARENABOTS[:0|1] -> CCGAME ARENABOTS:<on>
--
-- It sits on the Practice Battle line, where it has always been. It stays
-- hidden until the server answers, so a realm that does not fill arenas - or a
-- server built before this existed - keeps the stock frame and no empty box.
--
-- ArenaFrame.xml is Blizzard's and is not edited, so the box is built here,
-- the same way this file already builds the extra zone buttons. Everything
-- hangs off OnLoad: the <Script> tag runs this file BEFORE the frame exists,
-- so ArenaFrame is still nil at this point.

-- The vertical middle of ArenaFrameNameHeader2. The divider hangs off the
-- frame's LEFT at (14,95) and the header off the divider at (14,-9), which puts
-- the middle of that 14-pixel GameFontHighlight line here. A constant for the
-- same reason RATED_Y_TOP is one: neither ever moves.
local PRACTICE_HEADER_Y = -177

CENTURION_ARENA_BOTS_LABEL = "Arena bot matches"
CENTURION_ARENA_BOTS_TOOLTIP = "Arena bot matches\n\nWhen a skirmish queue has nobody left to match you with, it is filled out with bots so the match starts anyway.\n\nUncheck to hold your place until real opponents queue. Rated arenas are never filled either way."
CENTURION_ARENA_BOTS_RATED_TOOLTIP = "Arena bot matches\n\nWhen a skirmish or rated queue has nobody left to match you with, it is filled out with bots so the match starts anyway. A rated match against bots counts for your rating.\n\nUncheck to hold your place until real opponents queue. Rated then needs your whole team."

local function ArenaBots_Send(message)
    local playerName = UnitName("player")
    if SendAddonMessage and playerName then
        SendAddonMessage("CCGAMEREQ", message, "WHISPER", playerName)
    end
end

local function ArenaBots_Button()
    if _G.ArenaFrameArenaBots then
        return _G.ArenaFrameArenaBots
    end
    if not ArenaFrame then
        return nil
    end

    local cb = CreateFrame("CheckButton", "ArenaFrameArenaBots", ArenaFrame)
    cb:SetWidth(22)
    cb:SetHeight(22)
    cb:SetPoint("RIGHT", ArenaFrame, "TOPRIGHT", -40, PRACTICE_HEADER_Y)
    -- the label is part of the switch, as it is on the Battlegrounds tab
    cb:SetHitRectInsets(-110, 0, 0, 0)
    cb:SetNormalTexture("Interface\\Buttons\\UI-CheckBox-Up")
    cb:SetPushedTexture("Interface\\Buttons\\UI-CheckBox-Down")
    cb:SetHighlightTexture("Interface\\Buttons\\UI-CheckBox-Highlight", "ADD")
    cb:SetCheckedTexture("Interface\\Buttons\\UI-CheckBox-Check")
    cb:SetDisabledCheckedTexture("Interface\\Buttons\\UI-CheckBox-Check-Disabled")

    local text = cb:CreateFontString("ArenaFrameArenaBotsText", "ARTWORK", "GameFontNormalSmall")
    text:SetJustifyH("RIGHT")
    text:SetText(CENTURION_ARENA_BOTS_LABEL)
    text:SetPoint("RIGHT", cb, "LEFT", -1, 1)

    cb:SetScript("OnClick", function(self)
        if self:GetChecked() then
            ArenaBots_Send("ARENABOTS:1")
        else
            ArenaBots_Send("ARENABOTS:0")
        end
    end)
    cb:SetScript("OnEnter", function(self)
        GameTooltip:SetOwner(self, "ANCHOR_BOTTOMRIGHT", 5, 40)
        GameTooltip:SetText(CENTURION_ARENA_RATED_FILL and CENTURION_ARENA_BOTS_RATED_TOOLTIP or CENTURION_ARENA_BOTS_TOOLTIP,
            nil, nil, nil, nil, 1)
    end)
    cb:SetScript("OnLeave", GameTooltip_Hide)

    cb:Hide()
    return cb
end

local function ArenaBots_Ask()
    if ArenaBots_Button() then
        ArenaBots_Send("ARENABOTS")
    end
end

local ArenaBotsDriver = CreateFrame("Frame")
ArenaBotsDriver:RegisterEvent("PLAYER_ENTERING_WORLD")
ArenaBotsDriver:RegisterEvent("CHAT_MSG_ADDON")
ArenaBotsDriver:SetScript("OnEvent", function(self, event, prefix, message)
    if event == "PLAYER_ENTERING_WORLD" then
        ArenaBots_Ask()
        return
    end

    if prefix ~= "CCGAME" or type(message) ~= "string" then
        return
    end

    if message == "ARENAQUEUE:SHUFFLE" then
        CENTURION_ARENA_SHUFFLE = true
        CENTURION_ARENA_SQUADS = 0
        if ArenaFrame and ArenaFrame:IsShown() then
            ArenaFrame_Update()
        end
        return
    end

    local squads = string.match(message, "^ARENAQUEUE:(%d+)$")
    if squads then
        CENTURION_ARENA_SHUFFLE = false
        CENTURION_ARENA_SQUADS = tonumber(squads) or 0
        if ArenaFrame and ArenaFrame:IsShown() then
            ArenaFrame_Update()
        end
        return
    end

    local rated = string.match(message, "^ARENARATED:(%d)$")
    if rated then
        CENTURION_ARENA_RATED_FILL = rated == "1"
        if ArenaFrame and ArenaFrame:IsShown() then
            ArenaFrame_Update()
        end
        return
    end

    local on = string.match(message, "^ARENABOTS:(%d)$")
    if on then
        local cb = ArenaBots_Button()
        if cb then
            if on == "1" then
                cb:SetChecked(1)
            else
                cb:SetChecked(nil)
            end
            cb:Show()
        end
    end
end)

-- Wrapped rather than edited into ArenaFrame_OnLoad above, so the stock
-- function and this are one append apart and neither has to know the other.
local ArenaBots_PreviousOnLoad = ArenaFrame_OnLoad
function ArenaFrame_OnLoad(self)
    ArenaBots_PreviousOnLoad(self)
    ArenaBots_Button()
    -- Asked again whenever the battlemaster's window opens, so the box is right
    -- even if the login answer was missed.
    self:HookScript("OnShow", ArenaBots_Ask)
end
