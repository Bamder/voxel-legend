#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include "../core/gl.hpp"
#include "../core/math.hpp"
#include "../world/player.hpp"
#include "../world/world.hpp"
#include "../world/vitals.hpp"
#include "../world/target.hpp"

namespace anim { struct Clip; }
namespace mat { struct Model; }

enum class AppScreen {
    Playing = 0,
    Start,
    Worlds,
    WorldDetail,
    CreateWorld,
    RoomLobby,
    PlayerProfile,
    JoinRoom,
    RoomLoading
};

struct RemoteAvatar {
    uint32_t id = 0;
    std::string name;
    Vec3 pos{ 0, 0, 0 };
    float yaw = 0.0f;
    float pitch = 0.0f;
    float bodyYaw = 0.0f;
    float frame = 0.0f;
    float strikeFrame = 0.0f;
    uint8_t clip = 1;
    uint8_t strike = 0;
    uint8_t heldL = 0, heldR = 0, carried = 0;
    uint8_t wearU = 0, wearL = 0, wearS = 0;
    bool spectator = false;
    bool dead = false;
    bool hitFlash = false;
    uint8_t status = 0;
    vitals::Vitals vitals{};
};

struct ArcaneProjectileView {
    uint32_t id = 0, owner = 0;
    uint8_t kind = 1; // 1 fireball, 2 freeze
    Vec3 pos{}, vel{};
};

struct ArcaneBurstView {
    uint8_t kind = 1; // 1 fireball, 2 freeze, 3 heal
    Vec3 pos{};
    float age = 0.0f;
};

struct RoomTeamView {
    std::string name;
    float r = 1.0f, g = 1.0f, b = 1.0f;
    bool spectator = false;
};

struct RoomPlayerView {
    std::string name;
    int team = -1;
    bool local = false;
    bool host = false;
    uint32_t id = 0;
};

struct DeployPinView {
    uint32_t id = 0;
    std::string name;
    int bx = 0, bz = 0;
    uint8_t phase = 0;
    float t = 0.0f;
};

struct UIState {
    bool showDebug = false;
    bool inventoryOpen = false;
    int selectedLeft = 0;   // 0..HAND_SLOTS-1, left-hand hotbar
    int selectedRight = 0;  // 0..HAND_SLOTS-1, right-hand hotbar
    int selectedSlot = 3;   // absolute right-hand inventory index (HAND_SLOTS + selectedRight)
    const ItemSlot* inventory = nullptr; // 33 slots (left 3 + right 3 + main)
    const ItemSlot* wear = nullptr;      // wear::Count slots; open ones sit left of the body
    const ItemSlot* carrySlot = nullptr; // one-block carry (not part of the 6-slot bar)
    ItemSlot held;                       // item currently being dragged
    bool bagLocked = false;              // moving: backpack (non-hotbar) is grayed / unusable
    bool hasTarget = false;
    IVec3 targetBlock{ 0, 0, 0 };
    bool hasPlacePreview = false; // carried block, local placement ghost
    IVec3 placePreview{ 0, 0, 0 };
    int targetFace = 0;
    int targetPhys = -1;
    int targetDrop = -1;
    const std::vector<TrainingTarget>* targets = nullptr;
    int targetAim = -1;          // training dummy under the crosshair
    int targetPanel = -1;        // open status panel, index into targets
    int targetBtnHover = -1;     // 0 dismantle, 1 reset health
    bool targetPlaceReady = false;
    bool processLogReady = false; // hand axe on LOG: F to strip into WOOD
    int targetGuardian = -1; // relic index while the crosshair is on its guardian
    Vec3 guardianCenter{ 0, 0, 0 };
    Vec3 guardianSize{ 1, 1, 1 };
    float guardianHurt = 0.0f; // 0 intact, 1 empty
    bool bossNear = false;
    int bossRelic = -1;
    std::string bossName;
    int bossHp = 0;
    int bossMaxHp = 1;
    float breakProgress = 0.0f; // 0 intact .. 1 more cracks (targeted block)
    bool hasBreakOverlay = false;
    bool breakSod = false; // thinning sod overlay; dirt face stays drawn underneath

    float fps = 0.0f;
    int loadedChunks = 0;
    float timeOfDay = 0.0f;
    Vec3 playerPos{ 0, 0, 0 };
    Vec3 playerVel{ 0, 0, 0 };
    float yaw = 0.0f, pitch = 0.0f;
    bool flying = false, onGround = false;
    uint32_t seed = 0;

    float mouseX = 0.0f, mouseY = 0.0f;
    int hoveredSlot = -1;   // inventory slot under the mouse (-1 = none)
    int hoveredWear = -1;   // open wear slot under the mouse (-1 = none)
    int hoveredBlock = -1;  // palette block under the mouse (-1 = none)
    bool pointerInInventory = false; // mouse is over an inventory panel, not the dimmed world

    // Pause menu + settings.
    bool menuOpen = false;
    bool settingsOpen = false;
    float mouseSens = 0.0022f;
    bool invertY = false;
    int menuHover = -1;       // free play: 0 resume, 1 settings, 2 debug, 3 leave
                              // room match: 0 resume, 1 settings, 2 leave (debug hidden)
    int settingsHover = -1;   // 0 back, 1 invert-y toggle
    float sliderX = 0, sliderY = 0, sliderW = 0, sliderH = 0; // sensitivity slider rect

    // Debug menu (game-tick speed + time-of-day control).
    bool debugMenuOpen = false;
    float tickSpeed = 1.0f;   // game-tick multiplier (0 = pause, up to 20x)
    float tickSliderX = 0, tickSliderY = 0, tickSliderW = 0, tickSliderH = 0;
    float timeSliderX = 0, timeSliderY = 0, timeSliderW = 0, timeSliderH = 0;
    int debugHover = -1;      // 0 back, 1 humidity, 2 mat, 3 model, 4 dummy, 5 privilege, 6 fly, 7 trial space
    bool inTrial = false;
    bool trialPick = false;
    int trialHover = -1;      // trial list: -2 back, 0..14 relic
    int trialScroll = 0;
    bool railOpen = false;    // 权限模式右侧第二列
    int railHover = -1;       // 0 arrow, 1 guardian space, 2 fly, 3 quick break
    bool quickBreak = false;  // 权限模式：左键直接破坏方块
    float quickBreakWait = 0.0f; // seconds until the next quick break
    bool humidityMode = false; // render air as red/blue humidity blocks
    bool privilegeMode = false; // 权限模式 (debug): skip survival vitals / death
    float borderFog = 0.0f;     // 0..1 screen fog in the match rim
    float borderT = 0.0f;       // raw outward progress, 0..1
    bool borderActive = false;
    bool hideAvatar = false;
    bool structureEdit = false;
    bool blockBarOpen = false;
    int blockBarHover = -1;
    int blockBarScroll = 0;
    uint8_t structureBlock = PLANKS;
    bool structurePicker = false;
    bool structureNaming = false;
    bool structureCanReturn = false;
    int structureItemHover = -1;
    int structureDeleteHover = -1;
    int structureBtnHover = -1;   // list: 0 new, 1 back; naming: 0 field, 1 create, 2 back
    int structureOpHover = -1;    // 0 save, 1 switch file
    int structureScroll = 0;
    std::string structurePendingDelete;
    std::vector<std::string> structureNames;
    std::string structureNewName;
    std::string structureFile;
    std::string goalText;
    bool deploying = false;
    const std::vector<uint8_t>* deployPixels = nullptr;
    int deployStamp = 0;
    int deploySpan = 0;
    int deployOx = 0, deployOz = 0;
    float deployMapX = 0, deployMapY = 0, deployMapS = 0;
    float deployR = 1, deployG = 0.3f, deployB = 0.3f;
    int deploySeconds = -1;
    bool noteOpen = false;
    bool noteHover = false;
    bool noteBackHover = false;
    float noteX = 0, noteY = 0, noteW = 0, noteH = 0;
    float noteBackX = 0, noteBackY = 0, noteBackW = 0, noteBackH = 0;
    int noteRitual = -1;
    std::string noteTitle;
    std::string noteLines[8];
    int noteLineCount = 0;
    std::string noteItems[3];
    bool noteHeld[3] = { false, false, false };
    bool notePlaced[3] = { false, false, false };
    uint8_t noteItemId[3] = { 0, 0, 0 };
    bool noteDone = false;
    bool guideOpen = false;
    int guidePage = 0;
    int guidePageCount = 0;
    std::string guideTitle;
    std::string guideLines[7];
    int guideLineCount = 0;
    bool guidePrevHover = false;
    bool guideNextHover = false;
    bool guideCloseHover = false;
    bool clueOpen = false;
    bool clueTargetActive = false;
    int clueStage = 0;
    std::string clueDestination;
    std::string clueReward;
    Vec3 cluePosition{};
    bool clueBossRewardClaimed = false;
    bool clueCloseHover = false;
    bool storyOpen = false;
    bool storyHold = false;
    float storyFade = 1.0f;
    std::string storySentence;
    std::vector<DeployPinView> deployPins;

    const vitals::Vitals* vitals = nullptr;
    bool playerDead = false;
    float hitMarker = 0.0f;
    float damageFlash = 0.0f;
    uint8_t playerStatus = 0;
    std::vector<ArcaneProjectileView> arcaneProjectiles;
    std::vector<ArcaneBurstView> arcaneBursts;
    int deathHover = -1;      // 0 = respawn

    // Humidity label for the air block under the crosshair.
    bool hasHumidityBlock = false;
    IVec3 humidityBlock{ 0, 0, 0 };
    int humidityValue = 0;
    float humidityScreenX = 0, humidityScreenY = 0;

    // Material editor (texture painter).
    bool matEditorOpen = false;
    int camMode = 0;                  // 0 first-person, 1 third-person (over-shoulder), 2 second-person (front)
    bool netAnim = false;             // third person uses the server tick's frame id
    uint8_t netClip = 1;
    uint8_t netStrike = 0;
    float netFrame = 0.0f;
    float netStrikeFrame = 0.0f;
    float netYaw = 0.0f, netPitch = 0.0f, netBodyYaw = 0.0f;
    bool dummyActive = false;         // spawn an observation dummy player
    Vec3 dummyPos{ 0, 0, 0 };         // dummy's fixed position (set when spawned)
    bool dummyPlaced = false;
    int matEditorTile = TEX_DIRT;      // tile currently being edited
    float matEditorTileX = 0, matEditorTileY = 0, matEditorTileSize = 0;
    int matEditorHoverX = -1, matEditorHoverY = -1;
    float matEditorR = 0.0f, matEditorG = 1.0f, matEditorB = 0.0f, matEditorA = 1.0f;
    int matEditorPaletteHover = -1;
    bool matEditorDirty = false;

    // Title / world-select menus (shown before entering a world).
    AppScreen appScreen = AppScreen::Start;
    std::vector<std::string> worldNames;
    std::vector<std::string> backupNames;
    std::string selectedWorld;
    std::string newWorldName;
    std::string menuMessage;
    int selectedBackup = -1;
    int worldScroll = 0;
    int backupScroll = 0;
    int startHover = -1;       // 0 create room, 1 free explore (2..4 locked)
    // Create-room lobby. Team 0 is the spectator team when present.
    std::vector<RoomTeamView> roomTeams;
    std::vector<RoomPlayerView> roomPlayers;
    int roomMinPlayers = 1;
    int lobbyJoinHover = -1;   // team index of the "+" under the cursor
    int lobbyBtnHover = -1;    // 0 new team, 1 start, 2 back
    bool roomHost = false;     // this machine created the room and runs the server
    bool roomSession = false;  // in a room match: hide privilege mode and the debug panel
    int roomPort = 35535;
    std::string roomPortText = "35535";
    bool portFieldActive = false;
    bool portFieldHover = false;
    float portFieldX = 0, portFieldY = 0, portFieldW = 0, portFieldH = 0;
    std::string joinHost = "127.0.0.1";
    std::string joinPortText = "35535";
    int joinHover = -1;        // 0 address, 1 port, 2 connect, 3 back
    bool joinAddrActive = false;
    bool joinPortActive = false;
    std::string loadStatus;
    int loadHover = -1;        // 0 cancel
    std::vector<RemoteAvatar> remotes;
    bool spectating = false;   // in match, camera only
    std::string playerName = "玩家";
    bool portraitHover = false;
    float portraitX = 0.0f;
    int profileHover = -1;     // 0 name field, 1 import, 2 back
    bool menuWorld = false;    // free-explore backdrop
    Vec3 menuEye{ 0, 0, 0 };
    Vec3 menuTarget{ 0, 0, 0 };
    Vec3 menuFeet{ 0, 0, 0 };
    int worldItemHover = -1;
    int worldDeleteHover = -1; // row whose 删除 button is hovered
    int worldsBtnHover = -1;   // 0 create, 1 back
    int detailBtnHover = -1;   // 0 enter, 1 backup, 2 restore, 3 delete, 4 back
    bool deleteArmed = false;  // second click confirms delete
    std::string pendingDelete; // world name waiting for confirm on the list
    int backupItemHover = -1;
    int createBtnHover = -1;   // 0 name field, 1 create, 2 back
    bool nameFieldActive = false;
};

class Renderer {
public:
    bool init(int screenW, int screenH);
    void shutdown();
    void setScreenSize(int w, int h);
    void sync(const World& world);
    void render(const World& world, const Player& player, float timeOfDay, UIState& ui);
    void reloadPlayerAssets();

    struct Sky {
        Vec3 sunDir, moonDir, sunColor, ambient, fogColor, zenith, horizon, below, moonColor;
        float sunDisc, moonDisc, starAmount;
    };
    static void computeSky(float timeOfDay, Sky& s);

    // Text measurement (px) of a UTF-8 string at scale 1.0 (caches the glyph texture).
    void stringSize(const std::string& s, int& w, int& h);

    // Material editor: persist an edited tile image to disk + re-upload the atlas.
    void saveMaterialTile(int tile);

private:
    struct TextTex { unsigned int tex = 0; int w = 0, h = 0; };
    struct ChunkGL {
        unsigned int vaoO = 0, vboO = 0, vaoT = 0, vboT = 0;
        int opaqueCount = 0, transparentCount = 0;
        bool created = false;
    };
    struct UIBatch {
        std::vector<float> data;
        unsigned int vao = 0, vbo = 0;
    };

    unsigned int progWorld = 0, progSky = 0, progFlat = 0, progParticle = 0;
    unsigned int progUI = 0, progUIText = 0, progHum = 0, progHumTex = 0;
    int uMVP = 0, uChunkOffset = 0, uAtlas = 0, uSunDir = 0, uSunColor = 0, uAmbient = 0;
    int uFogColor = 0, uFogDensity = 0, uBlockScale = 0;
    int uBorderXZ = 0, uRimHalf = 0, uCameraPos = 0;
    int uSkyBorderXZ = 0, uSkyRimHalf = 0, uSkyCamera = 0;
    int uBreakRel = 0, uBreakProgress = 0, uBreakSod = 0, uBreakNrm = 0, uCrackReveal = 0;
    int uCrackFolds = 0, uCrackColor = 0, uCrackSeed = 0, uCrackShown = 0;
    int uCrackLenMu0 = 0, uCrackLenMu1 = 0, uCrackLenSig = 0;
    int uCrackWidMu0 = 0, uCrackWidMu1 = 0, uCrackWidSig = 0;
    int uCrackStep = 0, uCrackGaussZ = 0, uCrackEdge = 0, uCrackCorner = 0;
    int uInvVP = 0, uSkySunDir = 0, uSkyMoonDir = 0, uZenith = 0, uHorizon = 0, uBelow = 0;
    int uSkySunColor = 0, uSkyMoonColor = 0, uSunDisc = 0, uMoonDisc = 0, uStarAmount = 0;
    int uFlatMVP = 0, uFlatColor = 0;
    int uParticleMVP = 0, uParticleColor = 0, uParticleSoftness = 0, uParticleRing = 0;
    int uHumMVP = 0, uHumTexMVP = 0, uHumTexAtlas = 0;
    int uHumLit = 0, uHumSunDir = 0, uHumSunColor = 0, uHumAmbient = 0, uHumFogColor = 0, uHumFogDensity = 0;
    int uHumTexLit = 0, uHumTexSunDir = 0, uHumTexSunColor = 0, uHumTexAmbient = 0, uHumTexFogColor = 0, uHumTexFogDensity = 0;
    int uUIScreen = 0, uUITex = 0, uUITextScreen = 0, uUITextTex = 0;

    unsigned int atlasTex = 0, whiteTex = 0, cameraIconTex = 0, deployTex = 0;
    int deployTexStamp = -1;
    unsigned int skinTex = 0;
    std::unordered_map<int, unsigned int> garmentTex;
    std::unordered_map<std::string, unsigned int> overlayTex;
    std::unordered_map<std::string, unsigned int> extraMatTex;
    unsigned int skyVAO = 0, skyVBO = 0;
    unsigned int outlineVAO = 0, outlineVBO = 0;
    unsigned int particleVAO = 0, particleVBO = 0;
    unsigned int fallVAO = 0, fallVBO = 0;
    unsigned int humVAO = 0, humVBO = 0;
    unsigned int humTexVAO = 0, humTexVBO = 0;

    std::unordered_map<std::string, TextTex> m_textCache;
    std::unordered_map<int64_t, ChunkGL> m_chunkGL;
    UIBatch m_batch;
    std::vector<Vertex> m_fallMesh;

    int scrW = 1280, scrH = 720;

    void uploadChunk(ChunkGL& cg, const World::Chunk& ch);
    void destroyChunkGL(ChunkGL& cg);
    unsigned int renderTextTexture(const std::string& utf8, int& outW, int& outH);
    void drawSky(const Sky& s, const Mat4& invVP, const Vec3& eye = Vec3{},
                 float rimHalf = 0.0f, float bminX = 0.0f, float bmaxX = 0.0f,
                 float bminZ = 0.0f, float bmaxZ = 0.0f);
    void drawWorld(const World& w, const Vec3& eye, const Mat4& vp, const Sky& s,
                   const Vec3& breakRel, float breakProgress, float breakSod,
                   const Vec3& breakNrm, float fogDensity, const Vec3& fogColor,
                   float rimHalf = 0.0f, float bminX = 0.0f, float bmaxX = 0.0f,
                   float bminZ = 0.0f, float bmaxZ = 0.0f);
    void drawFallingTrees(const World& w, const Vec3& eye, const Mat4& vp, const Vec3& sunDir);
    void drawDrops(const World& w, const Vec3& eye, const Mat4& vp, const Sky& sky);
    void drawOutlineOriented(const Mat4& vp, const Vec3& eye, const PhysicsIsland& t,
                             int lx, int ly, int lz, float r, float g, float b, float a);
    void drawOutline(const Mat4& vp, const Vec3& eye, const IVec3& block,
                     float r, float g, float b, float a);
    void drawOutlineAt(const Mat4& vp, const Vec3& eye, const Vec3& center, const Vec3& size,
                       const Vec3& ax, const Vec3& ay, const Vec3& az,
                       float r, float g, float b, float a);
    void drawParticle(const Mat4& vp, const Vec3& eye, const Vec3& center,
                      float width, float height, float r, float g, float b, float a,
                      float softness = 0.3f, float ring = 0.0f);
    void drawBreakOverlay(const Mat4& vp, const Vec3& eye, const UIState& ui, const World& world);
    void drawCrackFace(const Mat4& vp, const Vec3& eye, const World& world,
                       int phys, int bx, int by, int bz, int face,
                       const float* steps, int nSteps);
    void drawHumidity(const World& w, const Vec3& eye, const Mat4& vp);
    void drawPlayerModel(const Vec3& pos, float bodyYaw, float headYaw, float pitch,
                         const Vec3& eye, const Mat4& vp, bool hideHead,
                         const anim::Clip* clip, float frame, uint8_t heldRight = AIR,
                         uint8_t heldLeft = AIR, uint8_t carried = AIR,
                         const vitals::Vitals* tint = nullptr,
                         const anim::Clip* strike = nullptr, float strikeAt = 0.0f,
                         const Sky* sun = nullptr,
                         uint8_t wearUpper = AIR, uint8_t wearLower = AIR, uint8_t wearShoes = AIR,
                         bool bare = false);
    void drawGuardians(const World& world, const Player& player, const UIState& ui,
                       const Vec3& eye, const Mat4& vp, const Sky* sun);
    void drawArcaneEffects(const Vec3& eye, const Mat4& vp, const Player& player,
                           const UIState& ui, bool firstPerson);
    void drawUI(const World& w, const Player& p, float timeOfDay, UIState& ui);
    void drawDeploy(UIState& ui);

    void quad(float x, float y, float w, float h, float u0, float v0, float u1, float v1,
              float r, float g, float b, float a);
    void scrimFade(float w, float h);
    void tri(float x0, float y0, float x1, float y1, float x2, float y2,
             float r, float g, float b, float a);
    void drawFlag(float x, float y, float w, float h, float r, float g, float b);
    void flushUI(unsigned int prog, unsigned int tex);
    void drawString(const std::string& s, float x, float y, float scale, float r, float g, float b, float a);
    void text(float x, float y, float scale, float r, float g, float b, float a, const char* fmt, ...);
    void centeredText(const std::string& s, float cx, float cy, float scale, float r, float g, float b, float a);

    // Crosshair hints, packed left to right from the right side of the crosshair.
    // A "/" is inserted between neighbors. Add future hints by appending another entry.
    enum class CrosshairKeyKind { Text, MouseRight };
    struct CrosshairPrompt {
        CrosshairKeyKind kind = CrosshairKeyKind::Text;
        const char* key = "";
        const char* action = "";
    };
    struct CrosshairPromptBox {
        float x = 0, y = 0, w = 0, h = 0;
        CrosshairKeyKind kind = CrosshairKeyKind::Text;
        std::string key, action;
    };
    void layoutCrosshairPrompts(const CrosshairPrompt* items, int count, float originX, float crossY,
                                std::vector<CrosshairPromptBox>& out);
    void drawCrosshairPromptChrome(const std::vector<CrosshairPromptBox>& boxes);
    void drawCrosshairPromptText(const std::vector<CrosshairPromptBox>& boxes);
    void drawBlockIcon(uint8_t block, float x, float y, float size);
    void drawModelItemIcon(uint8_t block, const mat::Model& model, float x, float y, float size);
    void drawHumSolid(const std::vector<float>& solid, const Mat4& mvp, const Sky* sun, float fogDensity);
    void buttonChrome(float x, float y, float w, float h, bool hovered,
                      float fr = 0.47f, float fg = 0.47f, float fb = 0.47f);
    void drawMenu(UIState& ui);
    void drawSettings(UIState& ui);
    void drawDebugMenu(UIState& ui);
    void drawStartMenu(UIState& ui);
    void drawPlayerProfile(UIState& ui);
    void drawRoomLobby(UIState& ui);
    void drawJoinRoom(UIState& ui);
    void drawRoomLoading(UIState& ui);
    void drawMenuPortrait(const World& world, float timeOfDay, UIState& ui);
    void drawWorldsMenu(UIState& ui);
    void drawStructurePicker(UIState& ui);
    void drawWorldDetail(UIState& ui);
    void drawCreateWorld(UIState& ui);
    void drawInventory(UIState& ui);
    void drawNote(UIState& ui);
    void drawGuide(UIState& ui);
    void drawClue(UIState& ui);
    void drawInventoryDoll(UIState& ui, float x, float y, float w, float h,
                           const vitals::Vitals* health = nullptr, bool showStamina = true);
    void drawTargetPanel(UIState& ui);
    void drawDeath(UIState& ui);
    void drawMaterialEditor(UIState& ui);
    void paletteLayout(int& x0, int& y0, int& cell, int& cols, int& rows) const;
};
