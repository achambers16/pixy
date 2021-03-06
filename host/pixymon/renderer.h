#ifndef RENDERER_H
#define RENDERER_H
#include <QObject>
#include <QImage>
#include "blobs.h"

#define LINE_COLOR 0xFF00FF2F   // bright green

class VideoWidget;

class Renderer : public QObject
{
    Q_OBJECT

public:
    Renderer(VideoWidget *video);
    ~Renderer();

    int render(uint32_t type, void *args[]);
    uint8_t *m_frameData;

    // experimental
    void setFilter(int16_t hmin, int16_t hmed, int16_t hmax, uint8_t smin, uint8_t smax, uint8_t vmin, uint8_t vmax, uint8_t cmin, uint8_t cmax);
    void setMode(uint32_t mode)
    {
        m_mode = mode;
    }

    Blobs m_blobs;

signals:
    void image(QImage image, bool blend);

private:
    inline void interpolateBayer(unsigned int width, unsigned int x, unsigned int y, unsigned char *pixel, unsigned int &r, unsigned int &g, unsigned int &b);

    int renderBA81(uint16_t width, uint16_t height, uint32_t frameLen, uint8_t *frame);	
    //int renderVISU(uint16_t width, uint16_t height, uint32_t frameLen, uint8_t *frame, uint32_t cc_num, int16_t* c_components);
    int renderVISU(uint32_t cc_num, int16_t* c_components);
    int renderCCQ1(uint16_t width, uint16_t height, uint32_t numVals, uint32_t *qVals);
    int renderCCB1(uint16_t width, uint16_t height, uint16_t numBlobs, uint16_t *blobs);

    int renderBA81Filter(uint16_t width, uint16_t height, uint32_t frameLen, uint8_t *frame);

    void handleRL(QImage *image, uint color, int row, int startCol, int len);

    VideoWidget *m_video;

    // experimental
    int16_t m_hmin;
    int16_t m_hmed;
    int16_t m_hmax;
    uint8_t m_smin;
    uint8_t m_smax;
    uint8_t m_vmin;
    uint8_t m_vmax;
    uint8_t m_cmin;
    uint8_t m_cmax;

    uint8_t *m_lut;
    uint32_t m_mode;
};

#endif // RENDERER_H

