#pragma once
#include <string>
#include <vector>
#include <random>
#include <sstream>

namespace NpcGen {

    const std::vector<std::string> human_first_names = {"Аларик", "Борн", "Валтер", "Гай", "Деррик", "Эйрик", "Каэль", "Морган", "Рен", "Торбин", "Элиза", "Лиара", "Сильвия", "Анна", "Мария", "Брунгильда", "Изольда"};
    const std::vector<std::string> human_last_names = {"Блэкуотер", "Свифт", "Айронсайд", "Кроу", "Вэнс", "Редфолл", "Сноу", "Грей", "Фокс", "Бладгуд"};
    
    const std::vector<std::string> dwarf_first_names = {"Брор", "Громли", "Торбин", "Магнар", "Дурин", "Хельга", "Дис", "Фрея"};
    const std::vector<std::string> dwarf_last_names = {"Камнерукий", "Громовой Молот", "Железнобокий", "Златобород", "Крепкощит"};
    
    const std::vector<std::string> elf_first_names = {"Эларион", "Фаэлан", "Иллидан", "Аравель", "Лираэль", "Элара", "Тиранда", "Сильвана"};
    const std::vector<std::string> elf_last_names = {"Сребролист", "Шепот Ветра", "Звездный Свет", "Лунный Тень", "Песнь Леса"};
    
    const std::vector<std::string> orc_first_names = {"Грошнак", "Крул", "Ургок", "Мазгак", "Гришак", "Бака", "Шива"};
    const std::vector<std::string> orc_last_names = {"Кровавый Клык", "Разрушитель", "Гнилая Пасть", "Костолом", "Черный Шрам"};

    const std::vector<std::string> backgrounds_poor = {
        "Родился в трущобах, с детства учился выживать, воруя хлеб.",
        "Сын разорившегося фермера. Землю забрали за долги, теперь ищет любую работу.",
        "Бывший раб, чудом сбежавший из шахт. Ненавидит власть и оковы.",
        "Сирота, выросший на улицах. Доверяет только звонкой монете.",
        "Его деревню сожгли бандиты. Он выжил, спрятавшись в колодце, и теперь живет одним днем."
    };

    const std::vector<std::string> backgrounds_middle = {
        "Подмастерье кузнеца, решивший, что достоин большего, чем махать молотом.",
        "Бывший стражник, уволенный за излишнюю жестокость к задержанным.",
        "Младший сын торговца, которому не досталось наследства. Пытается сколотить свой капитал.",
        "Дезертир из армии. Скрывает свое прошлое и вздрагивает от громких звуков.",
        "Обычный горожанин, чья жизнь перевернулась после встречи с культистами."
    };

    const std::vector<std::string> backgrounds_rich = {
        "Бастард знатного лорда. Имеет хорошее образование, но лишен прав на титул.",
        "Ученый-изгнанник, чьи эксперименты с Эфиром сочли слишком опасными.",
        "Обедневший аристократ, цепляющийся за остатки былой роскоши.",
        "Бывший член гильдии магов, исключенный за изучение запретных искусств."
    };

    const std::vector<std::string> backgrounds_insane = {
        "Слышит голоса из Великого Разлома. Утверждает, что огонь говорит с ним.",
        "Пережил Эфирную бурю без укрытия. Его разум расколот, а глаза ничего не выражают.",
        "Считает себя реинкарнацией древнего Архитектора. Одержим поиском 'Идеального Кода'."
    };

    inline std::string generateName(const std::string& factionId, std::mt19937& gen) {
        std::vector<std::string> first, last;
        if (factionId == "khazadrim") { first = dwarf_first_names; last = dwarf_last_names; }
        else if (factionId == "sylvanesti" || factionId == "greencode") { first = elf_first_names; last = elf_last_names; }
        else if (factionId == "gronnar") { first = orc_first_names; last = orc_last_names; }
        else { first = human_first_names; last = human_last_names; }
        
        std::string fn = first[gen() % first.size()];
        std::string ln = last[gen() % last.size()];
        return fn + " " + ln;
    }

    inline std::string generateBackground(int wealth_level, int paranoia, std::mt19937& gen) {
        if (paranoia > 80) return backgrounds_insane[gen() % backgrounds_insane.size()];
        if (wealth_level > 70) return backgrounds_rich[gen() % backgrounds_rich.size()];
        if (wealth_level > 30) return backgrounds_middle[gen() % backgrounds_middle.size()];
        return backgrounds_poor[gen() % backgrounds_poor.size()];
    }
}
