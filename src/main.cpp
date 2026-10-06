#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <sstream>
#include <vector>
#include <string>
#include <map>
#include <filesystem>
#include <fstream>
#include <algorithm>

using namespace geode::prelude;

struct RunRecord {
    int start;
    int end;
};

static int g_sessionStartPercent = 0;

static int calculatePlayerPercent(PlayLayer* pl) {
    if (!pl || !pl->m_player1) return 0;

    float levelLength = pl->m_levelLength;
    if (levelLength <= 0.0f) {
        return pl->getCurrentPercentInt();
    }

    float currentX = pl->m_player1->getPositionX();
    float pct = (currentX / levelLength) * 100.0f;
    int result = static_cast<int>(std::floor(pct));
    return std::clamp(result, 0, 100);
}

static std::filesystem::path getRunsFilePath() {
    auto dir = Mod::get()->getSaveDir();
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        std::filesystem::create_directories(dir, ec);
    }
    return dir / "runs.json";
}

static matjson::Value loadAllData() {
    auto path = getRunsFilePath();
    if (!std::filesystem::exists(path)) {
        return matjson::parse("{}").unwrapOrDefault();
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return matjson::parse("{}").unwrapOrDefault();
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    auto parseRes = matjson::parse(content);
    if (parseRes.isOk() && parseRes.unwrap().isObject()) {
        return parseRes.unwrap();
    }
    return matjson::parse("{}").unwrapOrDefault();
}

static void saveAllData(const matjson::Value& data) {
    auto path = getRunsFilePath();
    std::ofstream file(path);
    if (file.is_open()) {
        file << data.dump(matjson::NO_INDENTATION);
        file.close();
    }
}

static std::vector<RunRecord> loadRuns(int levelID) {
    std::vector<RunRecord> list;
    auto root = loadAllData();
    auto key = std::to_string(levelID);

    if (root.contains(key) && root[key].isArray()) {
        for (const auto& item : root[key].asArray().unwrap()) {
            if (item.isObject()) {
                int s = item["s"].asInt().unwrapOr(0);
                int e = item["e"].asInt().unwrapOr(0);
                list.push_back({s, e});
            }
        }
    }
    return list;
}

static void saveRun(int levelID, int start, int end) {
    if (end <= start && start == 0 && end == 0) return;

    auto runs = loadRuns(levelID);
    runs.push_back({start, end});

    std::string jsonArr = "[";
    for (size_t i = 0; i < runs.size(); ++i) {
        jsonArr += fmt::format(R"({{"s":{},"e":{}}})", runs[i].start, runs[i].end);
        if (i + 1 < runs.size()) {
            jsonArr += ",";
        }
    }
    jsonArr += "]";

    auto root = loadAllData();
    auto parsedArr = matjson::parse(jsonArr);
    if (parsedArr.isOk()) {
        root[std::to_string(levelID)] = parsedArr.unwrap();
        saveAllData(root);
    }
}

static std::string formatRuns(const std::vector<RunRecord>& runs) {
    if (runs.empty()) return "None";

    std::map<std::pair<int, int>, int> counts;
    std::vector<std::pair<int, int>> order;

    for (const auto& r : runs) {
        std::pair<int, int> p = {r.start, r.end};
        if (counts[p] == 0) {
            order.push_back(p);
        }
        counts[p]++;
    }

    std::ostringstream ss;
    for (size_t i = 0; i < order.size(); ++i) {
        auto p = order[i];
        int c = counts[p];

        if (p.first > 0) {
            ss << p.first << "-" << p.second << "%";
        } else {
            ss << p.second << "%";
        }

        if (c > 1) {
            ss << "x" << c;
        }

        if (i + 1 < order.size()) {
            ss << ", ";
        }
    }
    return ss.str();
}

class $modify(MyPlayLayer, PlayLayer) {
    struct Fields {
        bool m_recordedDeath = false;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        g_sessionStartPercent = 0;
        if (m_startPosObject != nullptr) {
            g_sessionStartPercent = calculatePlayerPercent(this);
        }
        m_fields->m_recordedDeath = false;
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();

        g_sessionStartPercent = 0;
        if (m_startPosObject != nullptr) {
            g_sessionStartPercent = calculatePlayerPercent(this);
        }
        m_fields->m_recordedDeath = false;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (!m_fields->m_recordedDeath) {
            m_fields->m_recordedDeath = true;

            int endPercent = calculatePlayerPercent(this);
            int startPercent = g_sessionStartPercent;

            if (m_level && m_level->m_levelID.value() > 0) {
                saveRun(m_level->m_levelID.value(), startPercent, endPercent);
            }
        }

        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        if (!m_fields->m_recordedDeath) {
            m_fields->m_recordedDeath = true;

            if (m_level && m_level->m_levelID.value() > 0) {
                saveRun(m_level->m_levelID.value(), g_sessionStartPercent, 100);
            }
        }
        PlayLayer::levelComplete();
    }
};

class $modify(MyLevelInfoLayer, LevelInfoLayer) {
    void onLevelInfo(CCObject* sender) {
        LevelInfoLayer::onLevelInfo(sender);

        if (!m_level || m_level->m_levelID.value() <= 0) return;

        auto runningScene = CCDirector::sharedDirector()->getRunningScene();
        if (!runningScene) return;

        FLAlertLayer* alert = nullptr;
        for (auto child : CCArrayExt<CCNode*>(runningScene->getChildren())) {
            if (auto alertLayer = typeinfo_cast<FLAlertLayer*>(child)) {
                alert = alertLayer;
            }
        }

        if (!alert) return;

        CCNode* container = alert->m_mainLayer ? static_cast<CCNode*>(alert->m_mainLayer) : static_cast<CCNode*>(alert);

        auto runs = loadRuns(m_level->m_levelID.value());
        std::string progressStr = "Progress: " + formatRuns(runs);

        auto label = CCLabelBMFont::create(progressStr.c_str(), "chatFont.fnt");
        label->setID("runs-progress-label"_spr);
        label->setColor({255, 255, 255});
        label->setOpacity(220);
        label->setAnchorPoint({0.5f, 0.5f});

        float maxTargetWidth = 260.0f;
        float defaultScale = 0.55f;
        if (label->getContentSize().width * defaultScale > maxTargetWidth) {
            label->setScale(maxTargetWidth / label->getContentSize().width);
        } else {
            label->setScale(defaultScale);
        }

        float lowestLabelY = 99999.0f;
        float btnY = 0.0f;
        bool foundBtn = false;

        for (auto child : CCArrayExt<CCNode*>(container->getChildren())) {
            if (auto bmFont = typeinfo_cast<CCLabelBMFont*>(child)) {
                if (bmFont->getPositionY() < lowestLabelY) {
                    lowestLabelY = bmFont->getPositionY();
                }
            } else if (auto menu = typeinfo_cast<CCMenu*>(child)) {
                btnY = menu->getPositionY();
                foundBtn = true;
            }
        }

        float adjustedY = 0.0f;
        if (lowestLabelY < 90000.0f && foundBtn) {
            adjustedY = (lowestLabelY + btnY) / 2.0f;
        } else if (lowestLabelY < 90000.0f) {
            adjustedY = lowestLabelY - 18.0f;
        } else {
            auto winSize = CCDirector::sharedDirector()->getWinSize();
            adjustedY = (winSize.height / 2.0f) - 58.0f;
        }

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        label->setPosition({winSize.width / 2.0f, adjustedY});

        container->addChild(label, 25);
    }
};
