#ifndef Sunrise_h
#define Sunrise_h

class Sunrise{
  public:
  Sunrise(float, float, float);
  void Actual();
  void Civil();
  void Nautical();
  void Astronomical();
  int Rise(unsigned char ,unsigned char );
  int Set(unsigned char ,unsigned char );
  int Noon(unsigned char ,unsigned char );
  unsigned char Hour();
  unsigned char Minute();
  
  private:
  int Compute(unsigned char ,unsigned char, int);
  float lat,lon, zenith, rd, tz;
  unsigned char  theHour,theMinute;
};

#endif