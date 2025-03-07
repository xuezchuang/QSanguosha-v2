#include "clientstruct.h"
#include "engine.h"
#include "settings.h"
#include "package.h"

ServerInfoStruct ServerInfo;

time_t ServerInfoStruct::getCommandTimeout(QSanProtocol::CommandType command, QSanProtocol::ProcessInstanceType instance)
{
    time_t timeOut;
    if (OperationTimeout == 0)
        return 0;
    else if (command == QSanProtocol::S_COMMAND_CHOOSE_GENERAL
        || command == QSanProtocol::S_COMMAND_ASK_GENERAL)
        timeOut = OperationTimeout * 15000;
    else if (command == QSanProtocol::S_COMMAND_SKILL_GUANXING
        || command == QSanProtocol::S_COMMAND_ARRANGE_GENERAL)
        timeOut = OperationTimeout * 2000;
    else if (command == QSanProtocol::S_COMMAND_NULLIFICATION)
        timeOut = NullificationCountDown * 1000;
    else
        timeOut = OperationTimeout * 1000;

    if (instance == QSanProtocol::S_SERVER_INSTANCE)
        timeOut += Config.S_SERVER_TIMEOUT_GRACIOUS_PERIOD;
    return timeOut;
}

bool ServerInfoStruct::parse(const QString &_str)
{
    if (_str.isEmpty()) {
        DuringGame = false;
    } else {
        DuringGame = true;

        QStringList str = _str.split(":");

        QString server_name = str.at(0);
        Name = QString::fromUtf8(QByteArray::fromBase64(server_name.toLatin1()));

        GameMode = str.at(1);
        if (GameMode.startsWith("02_1v1") || GameMode.startsWith("06_3v3")) {
            GameRuleMode = GameMode.mid(6);
            GameMode = GameMode.mid(0, 6);
        }
        OperationTimeout = str.at(2).toInt();
        NullificationCountDown = str.at(3).toInt();

        QStringList ban_packages = str.at(4).split("+");
        QList<const Package *> packages = Sanguosha->findChildren<const Package *>();
        foreach (const Package *package, packages) {
            if (package->inherits("Scenario"))
                continue;

            QString package_name = package->objectName();
            if (ban_packages.contains(package_name))
                package_name = "!" + package_name;

            Extensions << package_name;
        }

        QString flags = str.at(5);

        RandomSeat = flags.contains("R");
        EnableCheat = flags.contains("C");
        FreeChoose = EnableCheat && flags.contains("F");
        Enable2ndGeneral = flags.contains("S");
        EnableSame = flags.contains("T");
        EnableBasara = flags.contains("B");
        EnableHegemony = flags.contains("H");
        EnableAI = flags.contains("A");
        DisableChat = flags.contains("M");

        if (flags.contains("1"))
            MaxHpScheme = 1;
        else if (flags.contains("2"))
            MaxHpScheme = 2;
        else if (flags.contains("3"))
            MaxHpScheme = 3;
        else {
            MaxHpScheme = 0;
            for (char c = 'a'; c <= 'r'; c++) {
                if (flags.contains(c)) {
                    Scheme0Subtraction = int(c) - int('a') - 5;
                    break;
                }
            }
        }
    }

    return true;
}

ServerInfoWidget::ServerInfoWidget(bool show_lack)
{
    name_label = new QLabel;
    address_label = new QLabel;
    port_label = new QLabel;
    game_mode_label = new QLabel;
    player_count_label = new QLabel;
    two_general_label = new QLabel;
    same_label = new QLabel;
    basara_label = new QLabel;
    hegemony_label = new QLabel;
    random_seat_label = new QLabel;
    enable_cheat_label = new QLabel;
    free_choose_label = new QLabel;
    enable_ai_label = new QLabel;
    time_limit_label = new QLabel;
    max_hp_label = new QLabel;

    list_widget = new QListWidget;
    list_widget->setViewMode(QListView::IconMode);
    list_widget->setMovement(QListView::Static);

    QFormLayout *layout = new QFormLayout;
    layout->addRow(tr("Server name"), name_label);
    layout->addRow(tr("Address"), address_label);
    layout->addRow(tr("Port"), port_label);
    layout->addRow(tr("Game mode"), game_mode_label);
    layout->addRow(tr("Player count"), player_count_label);
    layout->addRow(tr("2nd general mode"), two_general_label);
    layout->addRow(tr("Same Mode"), same_label);
    layout->addRow(tr("Basara Mode"), basara_label);
    layout->addRow(tr("Hegemony Mode"), hegemony_label);
    layout->addRow(tr("Max HP scheme"), max_hp_label);
    layout->addRow(tr("Random seat"), random_seat_label);
    layout->addRow(tr("Enable cheat"), enable_cheat_label);
    layout->addRow(tr("Free choose"), free_choose_label);
    layout->addRow(tr("Enable AI"), enable_ai_label);
    layout->addRow(tr("Operation time"), time_limit_label);
    layout->addRow(tr("Extension packages"), list_widget);

    if (show_lack) {
        lack_label = new QLabel;
        layout->addRow(tr("Lack"), lack_label);
    } else
        lack_label = NULL;

    setLayout(layout);
}

void ServerInfoWidget::fill(const ServerInfoStruct &info, const QString &address)
{
    name_label->setText(info.Name);
    address_label->setText(address);
    game_mode_label->setText(Sanguosha->getModeName(info.GameMode));
    int player_count = Sanguosha->getPlayerCount(info.GameMode);
    player_count_label->setText(QString::number(player_count));
    port_label->setText(QString::number(Config.ServerPort));
    two_general_label->setText(info.Enable2ndGeneral ? tr("Enabled") : tr("Disabled"));
    same_label->setText(info.EnableSame ? tr("Enabled") : tr("Disabled"));
    basara_label->setText(info.EnableBasara ? tr("Enabled") : tr("Disabled"));
    hegemony_label->setText(info.EnableHegemony ? tr("Enabled") : tr("Disabled"));

    if (info.Enable2ndGeneral) {
        switch (info.MaxHpScheme) {
        case 0: max_hp_label->setText(QString(tr("Sum - %1")).arg(info.Scheme0Subtraction)); break;
        case 1: max_hp_label->setText(tr("Minimum")); break;
        case 2: max_hp_label->setText(tr("Maximum")); break;
        case 3: max_hp_label->setText(tr("Average")); break;
        }
    } else {
        max_hp_label->setText(tr("2nd general is disabled"));
        max_hp_label->setEnabled(false);
    }

    random_seat_label->setText(info.RandomSeat ? tr("Enabled") : tr("Disabled"));
    enable_cheat_label->setText(info.EnableCheat ? tr("Enabled") : tr("Disabled"));
    free_choose_label->setText(info.FreeChoose ? tr("Enabled") : tr("Disabled"));
    enable_ai_label->setText(info.EnableAI ? tr("Enabled") : tr("Disabled"));

    if (info.OperationTimeout == 0)
        time_limit_label->setText(tr("No limit"));
    else
        time_limit_label->setText(tr("%1 seconds").arg(info.OperationTimeout));

    list_widget->clear();

    static QIcon enabled_icon("image/system/enabled.png");
    static QIcon disabled_icon("image/system/disabled.png");

    foreach (QString extension, info.Extensions) {
        bool checked = !extension.startsWith("!");
        if (!checked)
            extension.remove("!");

        QString package_name = Sanguosha->translate(extension);
        QCheckBox *checkbox = new QCheckBox(package_name);
        checkbox->setChecked(checked);

        new QListWidgetItem(checked ? enabled_icon : disabled_icon, package_name, list_widget);
    }
}

void ServerInfoWidget::updateLack(int count)
{
    if (lack_label) {
        QString path = QString("image/system/number/%1.png").arg(count);
        lack_label->setPixmap(QPixmap(path));
    }
}

void ServerInfoWidget::clear()
{
    name_label->clear();
    address_label->clear();
    port_label->clear();
    game_mode_label->clear();
    player_count_label->clear();
    two_general_label->clear();
    same_label->clear();
    basara_label->clear();
    hegemony_label->clear();
    random_seat_label->clear();
    enable_cheat_label->clear();
    free_choose_label->clear();
    time_limit_label->clear();
    list_widget->clear();
}
