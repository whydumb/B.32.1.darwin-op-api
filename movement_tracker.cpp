#include <iostream>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>

using namespace mongocxx;

int main() {
    mongocxx::instance const inst{};
    
    mongocxx::client conn{mongocxx::uri{}};
    auto db = conn["movement_tracker"];
    auto collection = db["movementracker"];
    
    // 전체 문서 읽기 및 출력
    auto cursor = collection.find({});
    for (auto&& doc : cursor) {
        std::cout << bsoncxx::to_json(doc) << std::endl;
    }
    
    return 0;
}