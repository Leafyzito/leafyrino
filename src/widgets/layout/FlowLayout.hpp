// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QLayout>
#include <QStyle>

#include <vector>

namespace chatterino {

class FlowLayout : public QLayout
{
public:
    struct Options {
        int margin = -1;
        int hSpacing = -1;
        int vSpacing = -1;
    };

    explicit FlowLayout(QWidget *parent, Options options = {-1, -1, -1});
    explicit FlowLayout(Options options = {-1, -1, -1});

    ~FlowLayout() override;
    FlowLayout(const FlowLayout &) = delete;
    FlowLayout(FlowLayout &&) = delete;
    FlowLayout &operator=(const FlowLayout &) = delete;
    FlowLayout &operator=(FlowLayout &&) = delete;

    void addItem(QLayoutItem *item) override;

    void addLinebreak(int height = 0);

    [[nodiscard]] int horizontalSpacing() const;

    void setHorizontalSpacing(int value);

    [[nodiscard]] int verticalSpacing() const;

    void setVerticalSpacing(int value);

    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;

    QSize minimumSize() const override;
    QSize sizeHint() const override;

    void setGeometry(const QRect &rect) override;

    int count() const override;
    QLayoutItem *itemAt(int index) const override;

    QLayoutItem *takeAt(int index) override;

private:
    int doLayout(const QRect &rect, bool testOnly) const;

    int defaultSpacing(QStyle::PixelMetric pm) const;

    QSize getSpacing(QLayoutItem *item) const;

    std::vector<QLayoutItem *> itemList_;
    int hSpace_ = -1;
    int vSpace_ = -1;
};

}  // namespace chatterino
