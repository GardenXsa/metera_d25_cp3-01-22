#include <iostream>
#include <sstream>

class JsonWriter {
private:
    std::ostringstream ss;
    
public:
    void startObject() { ss << "{"; }
    void endObject() { ss << "}"; }
    
    void key(const std::string& k) {
        ss << "\"" << k << "\": ";
    }
    
    void value(const std::string& v) {
        ss << "\"" << v << "\"";
    }
    
    void comma() {
        ss << ",";
    }
    
    std::string str() const { return ss.str(); }
};

int main() {
    JsonWriter w;
    w.startObject();
    w.key("status"); w.value("ok");
    w.comma();
    w.key("message"); w.value("Engine initialized");
    w.endObject();
    std::cout << w.str() << std::endl;
    return 0;
}
