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
    
    // MongoDB 액션 -> Darwin-OP 명령 매핑
    std::map<std::string, std::string> action_mapping = {
        {"forward", "move_forward"},
        {"backward", "move_backward"},
        {"left", "turn_left"},
        {"right", "turn_right"}
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
