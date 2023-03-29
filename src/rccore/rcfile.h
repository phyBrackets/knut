#pragma once

#include "data.h"

#include <QStringList>

class QIODevice;

namespace RcCore {

struct RcFile
{
    QString fileName;
    QString content;
    bool isValid = false;

    // Global data
    QVector<Data::Include> includes;
    QHash<int, QString> resourceMap;

    // Data by languages
    QHash<QString, Data> data;
};

// Parse method
RcFile parse(const QString &fileName);

// Utility methods
void mergeAllLanguages(RcFile &rcFile, const QString &newLanguage = "default");

// Conversion methods
QVector<Asset> convertAssets(const Data &data, Asset::ConversionFlags flags = Asset::AllFlags);

Widget convertDialog(const Data &data, const Data::Dialog &dialog,
                     Widget::ConversionFlags flags = Widget::UpdateGeometry, double scaleX = 1.5, double scaleY = 1.65);

QVector<Action> convertActions(const Data &data, Asset::ConversionFlags flags = Asset::AllFlags);

// Write methods
void writeAssetsToImage(const QVector<Asset> &assets, Asset::TransparentColors colors = Asset::AllColors);

void writeAssetsToQrc(const QVector<Asset> &assets, QIODevice *device, const QString &fileName);

void writeDialogToUi(const Widget &widget, QIODevice *device);

} // namespace RcCore
