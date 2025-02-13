#include <stdio.h>
#include <stdlib.h>
#include <chrono>
#include <vector>
#include <math.h>
#include <ranges>

#include <portaudio.h>
#include <sndfile.h>
#include <SFML/Graphics.hpp>
#include <fftw3.h>

#define SCREEN_WIDTH 1920
#define SCREEN_HEIGHT 1080
#define FRAMES_PER_BUFFER 2048 // preferable power of 2
#define DOUBLE_AUDIBLE_BAND 44100
#define FFT_OUT_LENGTH 44100/2 + 1
#define REQUESTED_NUMBER_OF_POINTS 100

void printFileInfo(SF_INFO* info){
    printf("Sample Rate = %d Hz\n", info->samplerate);
    printf("Channels = %d\n", info->channels);
    printf("Format = 0x%x\n", info->format);
    printf("Sections = %d\n", info->sections);
    printf("Seekable = %d\n", info->seekable);
    printf("Frames = %ld\n", info->frames);
}

double ampTodB(double A){
    return 20 * log10(A);
}

double magnitude(fftw_complex c){
    return sqrt(pow(c[0],2) + pow(c[1],2));
}

int64_t getTime(){
    auto time = std::chrono::high_resolution_clock::now();
    auto dur = time.time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(dur).count();
    return ms;
}

std::vector<int> logspace(double start, double stop, int num) {
    std::vector<int> res(num);
    double step = (log10(stop) - log10(start)) / (num - 1);
    for (int i = 0; i < num; i++){
        res[i] = start * pow(10, i*step);
    }
    res.erase(unique(res.begin(), res.end()), res.end());
    return res;
}

typedef struct SongData{
    // Song attributes
    double* leftChannel;
    double* rightChannel;
    long int totalFrames;
    long int lastFrame;

    // For communication with graphics
    double soundLevel;
    std::vector<double> levels;
    fftw_complex fftOutLeft[FFT_OUT_LENGTH];
    fftw_complex fftOutRight[FFT_OUT_LENGTH];

    SongData(double* l, double* r, long int t){
        levels = std::vector<double>(FFT_OUT_LENGTH, 0.0);
        lastFrame = 0;
        soundLevel = 0;
        leftChannel = l;
        rightChannel = r;
        totalFrames = t;
    }
} SongData;

static int audioCallback(
    const void* inputBuffer,
    void* outputBuffer,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void* userData
){
    SongData* song = (SongData*)userData;
    float* out = (float*)outputBuffer;
    double sum = 0;

    // fft
    long int fftIdx;
    if (song->totalFrames - song->lastFrame < 44100){
        fftIdx = 44100;
    }else{
        fftIdx = song->lastFrame;
    }
    fftw_plan leftPlan = fftw_plan_dft_r2c_1d(
        DOUBLE_AUDIBLE_BAND,
        &song->leftChannel[fftIdx],
        song->fftOutLeft,
        FFTW_ESTIMATE
    );
    fftw_plan rightPlan = fftw_plan_dft_r2c_1d(
        DOUBLE_AUDIBLE_BAND,
        &song->rightChannel[fftIdx],
        song->fftOutRight,
        FFTW_ESTIMATE
    );
    fftw_execute(leftPlan); fftw_execute(rightPlan);
    for (int i = 0; i < FFT_OUT_LENGTH; i++){
        // song->levels[i] = (magnitude(song->fftOutLeft[i]) + magnitude(song->fftOutRight[i])) / 2;
        double dBLeft = ampTodB(magnitude(song->fftOutLeft[i]));
        double dBRight = ampTodB(magnitude(song->fftOutRight[i]));
        song->levels[i] = (dBLeft + dBRight)/2;
    }

    for (int i = 0; i < framesPerBuffer; i++){
        *out++ = song->leftChannel[song->lastFrame];
        *out++ = song->rightChannel[song->lastFrame];
        sum += abs((song->leftChannel[song->lastFrame]) + abs(song->rightChannel[song->lastFrame]))/2;
        song->lastFrame += 1;
    }
    song->soundLevel = sum/framesPerBuffer;
    return 0;
}

class BarVizualizer{
    private:
        std::vector<sf::RectangleShape> bars;
        std::vector<double> heights;
        double barWidth;
        double numBars;
        sf::RenderWindow* window;
    
    public:
        BarVizualizer(sf::RenderWindow* win, int n, double width, int screenHeight){
            barWidth = width;
            numBars = n;
            window = win;
            bars = std::vector<sf::RectangleShape>(numBars);
            heights = std::vector<double>(numBars, 0.f);
            for (int i = 0; i < numBars; i++){
                bars[i] = sf::RectangleShape(sf::Vector2f(barWidth, 0.f));
                bars[i].setOrigin(sf::Vector2f(-barWidth*i, -screenHeight));
                bars[i].setFillColor(sf::Color::Cyan);
            }
        }
        
        void setHeights(std::vector<int> indices, std::vector<double> levels){
            // numBars = length of indices
            for (int i = 0; i < numBars; i++){
                float curWidth = bars[i].getSize().x;
                bars[i].setSize(sf::Vector2f(curWidth, -levels[indices[i]] * 5));
            }
        }

        void draw(){
            for (int i = 0; i < numBars; i++){
                window->draw(bars[i]);
            }
        }
};

int main(void){
    SF_INFO info;
    SNDFILE* file  = sf_open("./music/snow.wav", SFM_READ, &info);
    printFileInfo(&info);
    const int samplerate = info.samplerate;
    const float duration = info.frames/samplerate;

    // Load entire song samples and close the file
    double* samples = static_cast<double*>(malloc((sizeof(double) * info.frames * info.channels)));
    double* leftChannel = static_cast<double*>(malloc((sizeof(double) * info.frames)));
    double* rightChannel = static_cast<double*>(malloc((sizeof(double) * info.frames)));
    int readcount = (int)sf_readf_double(file, samples, info.frames);
    for (int i = 0; i < readcount; i++){
        leftChannel[i] = samples[2*i];
        rightChannel[i] = samples[2*i+1];
    }
    free(samples);
    SongData song = SongData(leftChannel, rightChannel, info.frames);
    sf_close(file);

    // Start PA
    PaError err = Pa_Initialize();
    if(err != paNoError) Pa_Terminate();
    // Create PA stream
    PaStream* stream;
    err = Pa_OpenDefaultStream(
        &stream,
        0,
        2,
        paFloat32,
        samplerate,
        FRAMES_PER_BUFFER,
        audioCallback,
        &song
    );
    if(err != paNoError) Pa_Terminate();
    // Start PA stream
    err = Pa_StartStream(stream);
    if(err != paNoError) Pa_Terminate();
    int64_t startTime = getTime();

    // Graphics
    sf::RenderWindow window(sf::VideoMode(SCREEN_WIDTH, SCREEN_HEIGHT), "Vizualizer");
    std::vector<int> logLinearIndices = logspace(20, 20000, REQUESTED_NUMBER_OF_POINTS);
    int numBars = logLinearIndices.size();
    double barWidth = SCREEN_WIDTH/numBars;
    BarVizualizer viz = BarVizualizer(&window, numBars, barWidth, SCREEN_HEIGHT);
    // circle
    double r = 300;
    sf::CircleShape shape(300.f);
    shape.setOrigin(-SCREEN_WIDTH/2+r, -SCREEN_HEIGHT/2+r);
    shape.setFillColor(sf::Color::Green);
    float scale = 0;
    const float alpha = 0.5;
    while (getTime() - startTime < (duration-0.5)*1000){
        sf::Event event;
        while (window.pollEvent(event)){
            if (event.type == sf::Event::Closed){
                window.close();
                goto close;
            }
        }
        viz.setHeights(logLinearIndices, song.levels);
        window.clear();
        viz.draw();
        scale = (1-alpha)*scale + alpha*song.soundLevel;
        shape.setOrigin(-SCREEN_WIDTH/2+r*scale, -SCREEN_HEIGHT/2+r*scale);
        shape.setRadius(r*scale);
        window.draw(shape);
        window.display();
        // printf("1kHz dB = %f\n", song.levels[80]);
    }

close:
    // Stop and close stream
    err = Pa_StopStream(stream);
    if(err != paNoError) Pa_Terminate();
    err = Pa_CloseStream(stream);
    if(err != paNoError) Pa_Terminate();
    // Terminate
    Pa_Terminate();
    return err;
}