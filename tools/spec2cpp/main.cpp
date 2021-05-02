#include "specification.h"

#include <QDebug>
#include <QFile>
#include <QTextStream>

#include <optional>
#include <vector>

struct Data
{
    enum State {
        None,
        InNotification,
    };

    std::vector<State> state = {None};
    State currentState() const { return *state.rbegin(); }

    QString savedText;
};

// Global structure, this just make the code easier to write as you can always access them
Specification specs;
Data data;

static void readNone(const QString &line)
{
    if (line.startsWith("_Notification_")) {
        data.state.push_back(Data::InNotification);
        specs.notifications.push_back({});
    }
}

static std::optional<QString> extractMethod(const QString &line)
{
    if (!line.startsWith("* method:"))
        return {};

    auto split = line.split('\'', Qt::SkipEmptyParts);
    Q_ASSERT_X(split.size() >= 2, "notification", line.toLatin1());
    return split.at(1);
}

static QString decodeParam(const QString &param)
{
    if (param.startsWith("void") | param.startsWith("none") | param.startsWith("null"))
        return "std::nullptr_t";
    if (param == "number")
        return "int";
    if (param == "boolean")
        return "bool";
    if (param == "string")
        return "std::string";

    return param;
}

static std::optional<QString> specialParams(QString line)
{
    if (data.currentState() == Data::InNotification) {
        auto &notification = specs.currentNotification();
        if (notification.method == "telemetry/event") {
            Q_ASSERT(line.startsWith(R"('object' \| 'number' \| 'boolean' \| 'string')"));
            return "nlohmann::json";
        }
    }
    return {};
}
static std::optional<QString> extractParams(QString line)
{
    if (!line.startsWith("* params:"))
        return {};
    line = line.mid(10);

    if (auto specialParam = specialParams(line))
        return specialParam.value();

    // Extract all params
    auto params = line.split(R"(\|)");
    for (auto &param : params) {
        if (param.contains('`')) {
            int i1 = param.indexOf('`');
            int i2 = param.indexOf('`', i1 + 1);
            param = param.mid(i1 + 1, i2 - i1 - 1);
        } else if (param.contains('\'')) {
            int i1 = param.indexOf('\'');
            int i2 = param.indexOf('\'', i1 + 1);
            param = param.mid(i1 + 1, i2 - i1 - 1);
        } else {
            int i = param.indexOf(" ");
            param = param.left(i - 1);
        }
        param = decodeParam(param);
    }

    if (params.size() == 1)
        return params.first();
    else
        return QString("std::variant<%1>").arg(params.join(','));
}

static void readNotification(QString line)
{
    line = line.simplified();
    if (line.isEmpty())
        return;

    auto &notification = specs.currentNotification();

    if (auto method = extractMethod(line)) {
        notification.method = method.value();
    }
    if (auto params = extractParams(line)) {
        notification.params = params.value();
        data.state.pop_back();
    }
}

static void readSpecification(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly))
        return;

    QTextStream stream(&file);

    while (!stream.atEnd()) {
        QString line = stream.readLine();

        switch (data.currentState()) {
        case Data::None:
            readNone(line);
            break;
        case Data::InNotification:
            readNotification(line);
            break;
        }
    }
}

static void printSpecification()
{
    for (const auto &notification : specs.notifications) {
        qDebug().noquote() << QString("Notification(%1, %2)").arg(notification.method, notification.params);
    }
}

int main(int argc, char *argv[])
{
    Q_UNUSED(argc);
    Q_UNUSED(argv);
    readSpecification(":/specification-3-16.md");
    printSpecification();
}
