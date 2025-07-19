#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <map>
#include <bsoncxx/json.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/stdx.hpp>

// Windows에서 HTTP 요청을 위한 헤더 (libcurl 대신 간단한 방법)
#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")
#else
#include <curl/curl.h>
#endif

using namespace mongocxx;
using namespace bsoncxx::builder::stream;

class DarwinOpController {
private:
    std::string robot_ip;
    int robot_port;
    std::string last_action;
    
    // MongoDB 액션 -> Darwin-OP 명령 매핑 (stop/start 제거)
    std::map<std::string, std::string> action_mapping = {
        {"forward", "move_forward"},
        {"backward", "move_backward"},
        {"left", "turn_left"},
        {"right", "turn_right"},
        {"idle", "walk_stop"}
    };

public:
    DarwinOpController(const std::string& ip, int port = 8080) 
        : robot_ip(ip), robot_port(port) {
        std::cout << "[ROBOT] Darwin-OP 제어 대상: http://" << robot_ip << ":" << robot_port << std::endl;
    }
    
    bool send_http_command(const std::string& command) {
#ifdef _WIN32
        // Windows에서 WinINet 사용 (libcurl 없이)
        HINTERNET hInternet = InternetOpen(L"MovementTracker", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
        if (!hInternet) {
            std::cout << "[HTTP] ❌ 인터넷 초기화 실패" << std::endl;
            return false;
        }
        
        std::string url = "http://" + robot_ip + ":" + std::to_string(robot_port) + "/?command=" + command;
        std::wstring wurl(url.begin(), url.end());
        
        HINTERNET hConnect = InternetOpenUrl(hInternet, wurl.c_str(), NULL, 0, 
            INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
        
        bool success = false;
        if (hConnect) {
            char buffer[1024];
            DWORD bytesRead;
            if (InternetReadFile(hConnect, buffer, sizeof(buffer) - 1, &bytesRead)) {
                buffer[bytesRead] = '\0';
                if (strstr(buffer, "200 OK") || bytesRead > 0) {
                    std::cout << "[HTTP] ✅ 명령 전송 성공: " << command << std::endl;
                    success = true;
                } else {
                    std::cout << "[HTTP] ❌ 응답 오류: " << command << std::endl;
                }
            }
            InternetCloseHandle(hConnect);
        } else {
            std::cout << "[HTTP] ❌ 연결 실패: " << command << std::endl;
        }
        
        InternetCloseHandle(hInternet);
        return success;
        
#else
        // Linux에서 libcurl 사용
        CURL* curl = curl_easy_init();
        if (!curl) return false;
        
        std::string url = "http://" + robot_ip + ":" + std::to_string(robot_port) + "/?command=" + command;
        std::string response;
        
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, 
            [](void* contents, size_t size, size_t nmemb, std::string* s) -> size_t {
                s->append((char*)contents, size * nmemb);
                return size * nmemb;
            });
        
        CURLcode res = curl_easy_perform(curl);
        bool success = (res == CURLE_OK);
        
        if (success) {
            std::cout << "[HTTP] ✅ 명령 전송 성공: " << command << std::endl;
        } else {
            std::cout << "[HTTP] ❌ 명령 전송 실패: " << command << std::endl;
        }
        
        curl_easy_cleanup(curl);
        return success;
#endif
    }
    
    bool execute_action(const std::string& mongo_action) {
        // 중복 액션 방지
        if (mongo_action == last_action) {
            return true;
        }
        
        auto it = action_mapping.find(mongo_action);
        if (it == action_mapping.end()) {
            std::cout << "[ROBOT] ⚠️ 알 수 없는 액션: " << mongo_action << std::endl;
            return false;
        }
        
        bool success = send_http_command(it->second);
        if (success) {
            last_action = mongo_action;
        }
        
        return success;
    }
    
    const std::string& get_last_action() const {
        return last_action;
    }
};

class MongoDBTracker {
private:
    mongocxx::client client;
    mongocxx::database db;
    mongocxx::collection collection;

public:
    MongoDBTracker() : client{mongocxx::uri{}}, db{client["movement_tracker"]}, collection{db["movementracker"]} {
        std::cout << "[MONGO] MongoDB 연결 완료" << std::endl;
        std::cout << "[MONGO] 대상: movement_tracker.movementracker" << std::endl;
    }
    
    void print_all_documents() {
        std::cout << "\n=== 전체 MongoDB 문서 (기존 코드와 동일) ===\n";
        // 기존 movement_tracker.cpp와 정확히 동일한 코드
        auto cursor = collection.find({});
        int count = 0;
        for (auto&& doc : cursor) {
            std::cout << "문서 " << ++count << ": " << bsoncxx::to_json(doc) << std::endl;
        }
        std::cout << "총 " << count << "개 문서\n" << std::endl;
    }
    
    void print_tracking_documents_only() {
        std::cout << "\n=== 추적 중인 문서만 출력 ===\n";
        try {
            auto filter = document{} << "status" << "tracking" << finalize;
            auto cursor = collection.find(filter.view());
            int count = 0;
            
            for (auto&& doc : cursor) {
                count++;
                auto view = doc.view();
                
                std::cout << "--- 추적 문서 " << count << " ---" << std::endl;
                std::cout << "전체 JSON: " << bsoncxx::to_json(view) << std::endl;
                
                // 핵심 필드들 추출해서 보기 쉽게 출력
                std::cout << "📋 핵심 정보:" << std::endl;
                
                if (view["current_action"]) {
                    std::cout << "  현재 액션: " << view["current_action"].get_utf8().value.to_string() << std::endl;
                }
                
                if (view["total_actions"]) {
                    std::cout << "  총 액션 수: " << view["total_actions"].get_int32().value << std::endl;
                }
                
                if (view["current_yaw"]) {
                    if (view["current_yaw"].type() == bsoncxx::type::k_double) {
                        std::cout << "  현재 방향(Yaw): " << view["current_yaw"].get_double().value << std::endl;
                    } else if (view["current_yaw"].type() == bsoncxx::type::k_int32) {
                        std::cout << "  현재 방향(Yaw): " << view["current_yaw"].get_int32().value << std::endl;
                    }
                }
                
                if (view["player_id"]) {
                    std::cout << "  플레이어 ID: " << view["player_id"].get_utf8().value.to_string() << std::endl;
                }
                
                if (view["replay_name"]) {
                    std::cout << "  리플레이 이름: " << view["replay_name"].get_utf8().value.to_string() << std::endl;
                }
                
                std::cout << std::endl;
            }
            
            if (count == 0) {
                std::cout << "⚠️ status='tracking'인 문서가 없습니다." << std::endl;
                std::cout << "💡 MongoDB에서 tracking 상태인 데이터를 생성해주세요." << std::endl;
            } else {
                std::cout << "총 " << count << "개의 추적 중인 문서" << std::endl;
            }
            
        } catch (const std::exception& e) {
            std::cout << "[MONGO] ❌ 추적 문서 조회 실패: " << e.what() << std::endl;
        }
        std::cout << std::endl;
    }
    
    bsoncxx::stdx::optional<bsoncxx::document::value> get_current_tracking() {
        try {
            // 가장 최근 데이터
            auto opts = mongocxx::options::find{};
            opts.sort(document{} << "_id" << -1 << finalize);
            opts.limit(1);
            
            auto cursor = collection.find({}, opts);
            auto it = cursor.begin();
            
            if (it != cursor.end()) {
                return bsoncxx::document::value{*it};
            }
        } catch (const std::exception& e) {
            std::cout << "[MONGO] ❌ 현재 데이터 조회 실패: " << e.what() << std::endl;
        }
        
        return {};
    }
    
    // 실시간 모니터링을 위한 함수
    void monitor_changes(int seconds = 10) {
        std::cout << "\n🔍 " << seconds << "초간 MongoDB 변화 모니터링..." << std::endl;
        std::cout << "status='tracking'인 문서의 total_actions 변화를 감지합니다.\n" << std::endl;
        
        int last_total_actions = -1;
        std::string last_action = "";
        
        for (int i = 0; i < seconds; i++) {
            auto data = get_current_tracking();
            
            if (data) {
                auto view = data->view();
                
                int current_total = view["total_actions"] ? 
                    view["total_actions"].get_int32().value : 0;
                
                std::string current_action = view["current_action"] ? 
                    view["current_action"].get_utf8().value.to_string() : "unknown";
                
                if (current_total != last_total_actions || current_action != last_action) {
                    std::cout << "[" << i+1 << "초] 📊 변화 감지!" << std::endl;
                    std::cout << "  액션: " << last_action << " → " << current_action << std::endl;
                    std::cout << "  총 액션: " << last_total_actions << " → " << current_total << std::endl;
                    
                    last_total_actions = current_total;
                    last_action = current_action;
                } else {
                    std::cout << "[" << i+1 << "초] 변화 없음 (액션: " << current_action 
                              << ", 총: " << current_total << ")" << std::endl;
                }
            } else {
                std::cout << "[" << i+1 << "초] tracking 데이터 없음" << std::endl;
            }
            
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        std::cout << "\n✅ 모니터링 완료!" << std::endl;
    }
};

class SimpleSync {
private:
    DarwinOpController robot;
    MongoDBTracker tracker;
    bool running;
    int last_total_actions;
    int sync_count;

public:
    SimpleSync(const std::string& robot_ip) 
        : robot(robot_ip), running(false), last_total_actions(0), sync_count(0) {
    }
    
    void run_sync_loop() {
        std::cout << "\n🚀 MongoDB → Darwin-OP 동기화 시작!" << std::endl;
        std::cout << "Ctrl+C로 중지하세요.\n" << std::endl;
        
        running = true;
        int no_data_count = 0;
        
        while (running) {
            try {
                auto data = tracker.get_current_tracking();
                
                if (!data) {
                    no_data_count++;
                    if (no_data_count % 10 == 1) {  // 10초마다 메시지
                        std::cout << "[SYNC] 데이터 대기 중..." << std::endl;
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
                
                no_data_count = 0;
                auto doc_view = data->view();
                
                // 필드 추출
                int current_total = doc_view["total_actions"] ? 
                    doc_view["total_actions"].get_int32().value : 0;
                
                std::string current_action = doc_view["current_action"] ? 
                    doc_view["current_action"].get_utf8().value.to_string() : "idle";
                
                // 새로운 액션 확인
                if (current_total > last_total_actions) {
                    std::cout << "[SYNC] 📡 새 액션 감지: " << current_action 
                              << " (총 " << current_total << "개)" << std::endl;
                    
                    bool success = robot.execute_action(current_action);
                    
                    if (success) {
                        last_total_actions = current_total;
                        sync_count++;
                        std::cout << "[SYNC] ✅ 동기화 #" << sync_count << " 완료!" << std::endl;
                        
                        // 성공 후 잠시 대기 (단일 스레드 HTTP 서버 고려)
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    } else {
                        std::cout << "[SYNC] ❌ 명령 전송 실패, 재시도 대기..." << std::endl;
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                    }
                } else {
                    // 새 액션이 없으면 여유롭게 대기
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                
            } catch (const std::exception& e) {
                std::cout << "[SYNC] ❌ 오류: " << e.what() << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
        
        std::cout << "\n🛑 동기화 종료!" << std::endl;
    }
    
    void stop() {
        running = false;
    }
    
    void print_status() {
        auto data = tracker.get_current_tracking();
        
        std::cout << "\n📊 현재 상태:" << std::endl;
        std::cout << "  동기화 횟수: " << sync_count << std::endl;
        std::cout << "  마지막 액션: " << (robot.get_last_action().empty() ? "없음" : robot.get_last_action()) << std::endl;
        std::cout << "  처리된 총 액션: " << last_total_actions << std::endl;
        
        if (data) {
            auto doc_view = data->view();
            std::string current_action = doc_view["current_action"] ? 
                doc_view["current_action"].get_utf8().value.to_string() : "unknown";
            int total_actions = doc_view["total_actions"] ? 
                doc_view["total_actions"].get_int32().value : 0;
            
            std::cout << "  현재 MongoDB 액션: " << current_action << std::endl;
            std::cout << "  MongoDB 총 액션: " << total_actions << std::endl;
        }
        std::cout << std::endl;
    }
};

int main() {
    std::cout << "🤖 MongoDB → Darwin-OP 동기화 시스템 (C++)" << std::endl;
    std::cout << "=" << std::string(50, '=') << std::endl;
    
    // MongoDB 인스턴스 초기화 (기존 코드와 동일)
    mongocxx::instance const inst{};
    
    // 로봇 IP 입력받기
    std::string robot_ip;
    std::cout << "\n🤖 Darwin-OP 로봇 IP 주소를 입력하세요 (예: 192.168.1.100): ";
    std::cin >> robot_ip;
    std::cout << "설정된 로봇 IP: " << robot_ip << std::endl;
    
    try {
        MongoDBTracker tracker;
        
        std::cout << "\n🎮 명령어:" << std::endl;
        std::cout << "  3 - Darwin-OP 동기화 시작" << std::endl;
        std::cout << "  q - 종료" << std::endl;
        
        SimpleSync sync_manager(robot_ip);
        char choice;
        
        while (true) {
            std::cout << "\n> ";
            std::cin >> choice;
            
            if (choice == 'q' || choice == 'Q') {
                break;
            }
            else if (choice == '3') {
                sync_manager.run_sync_loop();  // 이 함수는 무한 루프 (Ctrl+C로 중지)
            }
            else {
                std::cout << "알 수 없는 명령어입니다." << std::endl;
            }
        }
        
    } catch (const std::exception& e) {
        std::cout << "❌ 오류: " << e.what() << std::endl;
        return 1;
    }
    
    std::cout << "👋 프로그램을 종료합니다." << std::endl;
    return 0;
}
