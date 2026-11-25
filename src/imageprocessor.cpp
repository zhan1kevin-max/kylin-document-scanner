#include "imageprocessor.h"

#include <QPainter>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace scan {
namespace {

cv::Mat rgbCopy(const QImage &image)
{
    if (image.isNull())
        throw std::runtime_error("图像为空，无法处理。");
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    if (rgb.isNull())
        throw std::runtime_error("图像转换失败，可能没有足够内存。");
    return cv::Mat(rgb.height(), rgb.width(), CV_8UC3,
                   const_cast<uchar *>(rgb.constBits()), rgb.bytesPerLine()).clone();
}

QImage imageCopy(const cv::Mat &matrix)
{
    if (matrix.empty())
        throw std::runtime_error("图像处理未生成有效结果。");
    const QImage::Format format = matrix.channels() == 1
        ? QImage::Format_Grayscale8 : QImage::Format_RGB888;
    QImage result(matrix.data, matrix.cols, matrix.rows,
                  static_cast<int>(matrix.step), format);
    result = result.copy();
    if (result.isNull())
        throw std::runtime_error("无法保存处理后的图像，可能没有足够内存。");
    return result;
}

double cross(const QPointF &a, const QPointF &b, const QPointF &c)
{
    return (b.x() - a.x()) * (c.y() - b.y())
         - (b.y() - a.y()) * (c.x() - b.x());
}

using Quad = std::array<cv::Point2f, 4>;
using Triangle = std::array<cv::Point2f, 3>;

double length(cv::Point2f v) { return std::hypot(double(v.x), double(v.y)); }
double determinant(cv::Point2f a, cv::Point2f b) { return a.x*b.y-a.y*b.x; }

Quad ordered(Quad points)
{
    cv::Point2f center(0,0);
    for (const auto &p : points) center += p * 0.25f;
    std::sort(points.begin(), points.end(), [center](auto a, auto b) {
        return std::atan2(a.y-center.y,a.x-center.x) < std::atan2(b.y-center.y,b.x-center.x);
    });
    auto first = std::min_element(points.begin(),points.end(),[](auto a, auto b) {
        return a.x+a.y < b.x+b.y;
    });
    std::rotate(points.begin(),first,points.end());
    return points;
}

struct Candidate {
    Quad corners;
    double score = 0.0;
    bool hasCut = false;
    Triangle cut{};
};

constexpr double cornerOvershoot = 0.25;

class SheetLocator {
public:
    explicit SheetLocator(const cv::Mat &rgb): color(rgb)
    {
        cv::cvtColor(rgb, gray, cv::COLOR_RGB2GRAY);
        std::vector<cv::Mat> channels;
        cv::split(rgb, channels);
        // Minimum channel disambiguates white paper on ochre/wood surfaces
        // even when paper and desk have nearly the same luminance.
        cv::min(channels[0], channels[1], tone);
        cv::min(tone, channels[2], tone);
        cv::GaussianBlur(tone,tone,{3,3},0.8);
        cv::GaussianBlur(gray,gray,{3,3},0.8);
        cv::Mat luminanceEdges;
        cv::Canny(gray,luminanceEdges,15,45);
        cv::Canny(tone,edges,15,45);
        cv::bitwise_or(edges,luminanceEdges,edges);
    }

    QVector<QPointF> locate(QVector<QPointF> *missingTriangle)
    {
        const auto kernel = cv::getStructuringElement(cv::MORPH_RECT, {5,5});
        cv::Mat mask;
        cv::morphologyEx(edges,mask,cv::MORPH_CLOSE,kernel);
        candidatesFromMask(mask);
        cv::threshold(tone,mask,0,255,cv::THRESH_BINARY|cv::THRESH_OTSU);
        cv::morphologyEx(mask,mask,cv::MORPH_CLOSE,kernel);
        candidatesFromMask(mask);
        if (best.score < 0.73) {
            // Weak gradients from illumination or shadow can leave a contour
            // open. Long side intersections provide an independent proposal.
            candidatesFromLines();
        }
        if (best.score < 0.51) return {};
        refineBoundary();
        if(!best.hasCut)findCutOnOutline();
        auto normalize=[this](cv::Point2f p){return QPointF(p.x/(gray.cols-1.0),p.y/(gray.rows-1.0));};
        QVector<QPointF> result;
        for (auto point : best.corners) result.append(normalize(point));
        if(missingTriangle && best.hasCut){
            for(auto p:best.cut)missingTriangle->append(normalize(p));
            if(!ImageProcessor::validMissingTriangle(result,*missingTriangle))missingTriangle->clear();
        }
        return result;
    }

private:
    struct Line {
        cv::Point2f start, direction;
        double extent;
    };
    cv::Mat color, gray, tone, edges;
    Candidate best;
    std::vector<Line> contourEdges;

    bool onSensor(cv::Point2f p) const
    {
        return p.x>=0 && p.y>=0 && p.x<=gray.cols-1 && p.y<=gray.rows-1;
    }

    static int sample(const cv::Mat &m, cv::Point2f p)
    {
        return m.at<uchar>(std::clamp(cvRound(p.y),0,m.rows-1),
                           std::clamp(cvRound(p.x),0,m.cols-1));
    }

    void consider(const Quad &unordered, const Triangle *cut = nullptr)
    {
        const auto q = ordered(unordered);
        std::array<double,4> sides{};
        double area = 0.0, worstCos = 0.0;
        int outside=0;
        for (int i=0;i<4;++i) {
            auto a=q[i], b=q[(i+1)%4], c=q[(i+2)%4];
            if(!std::isfinite(a.x) || !std::isfinite(a.y))return;
            if(!onSensor(a)){
                if(++outside>1 || a.x < -(gray.cols-1)*cornerOvershoot ||
                   a.y < -(gray.rows-1)*cornerOvershoot || a.x > (gray.cols-1)*(1+cornerOvershoot) ||
                   a.y > (gray.rows-1)*(1+cornerOvershoot))return;
            }else if(a.x<1 || a.y<1 || a.x>gray.cols-2 || a.y>gray.rows-2){
                // Frame-clipped contour endpoints are not physical paper corners.
                return;
            }
            auto u=b-a, v=c-b;
            if (determinant(u,v)<=0) return;
            sides[i]=length(u);
            if (sides[i] < std::min(gray.cols,gray.rows)*0.13) return;
            worstCos=std::max(worstCos,std::abs(u.dot(v))/(sides[i]*length(v)));
            area+=determinant(a,b)*0.5;
        }
        const double coverage=area/gray.total();
        if (coverage<0.09 || coverage>0.985 || worstCos>0.72) return;
        const double aspect=(sides[0]+sides[2])/(sides[1]+sides[3]);
        const double axisRatio=std::max(aspect,1.0/aspect);
        if (axisRatio>2.45) return;
        const double ratioFit=std::exp(-2.3*std::abs(std::log(axisRatio/std::sqrt(2.0))));
        if(outside && (worstCos>.50 || ratioFit<.55))return;
        // Sample away from printed borders. A dark rectangular table or a
        // thin printed outline has little sustained paper/desk step across
        // both offsets, even when its contour is larger and straighter.
        const float nearOffset=std::clamp(float(std::sqrt(area)*0.012),2.6f,6.5f);
        const float farOffset=nearOffset*2.2f;
        double bothPolarities[2] = {};
        int strongSides[2] = {};
        int nearPaper=0, nearSamples=0;
        for (int i=0;i<4;++i) {
            auto a=q[i], b=q[(i+1)%4], d=b-a;
            const cv::Point2f normal(-d.y/sides[i],d.x/sides[i]);
            int votes[2]={}, visible=0;
            constexpr int points=28;
            for (int j=0;j<points;++j) {
                auto pos=a+d*float(0.07+0.86*(j+.5)/points);
                // A real document may lie a few pixels from the image border.
                // Shorten the normal sample while keeping both measurements
                // on sensor pixels. Never synthesize the missing outside.
                if(!onSensor(pos))continue;
                float far=farOffset;
                while(far>=2.0f && (!onSensor(pos-normal*far) || !onSensor(pos+normal*far)))far*=0.65f;
                if(far<2.0f) continue;
                const float near=std::min(nearOffset,far*.70f);
                const auto inside=pos+normal*far;
                const int light=sample(gray,inside), paperTone=sample(tone,inside);
                if(light>=82 && light-paperTone<44 && paperTone>=62) ++nearPaper;
                ++nearSamples;
                ++visible;
                auto delta=[&](const cv::Mat &m,float offset){
                    return sample(m,pos+normal*offset)-sample(m,pos-normal*offset);
                };
                for(int k=0;k<2;++k) {
                    const int sign=k? -1:1;
                    const int colorStep=std::min(sign*delta(tone,near),sign*delta(tone,far));
                    const int lumaStep=std::min(sign*delta(gray,near),sign*delta(gray,far));
                    if(std::max(colorStep,lumaStep)>=6) ++votes[k];
                }
            }
            // Close-to-frame paper is valid if its boundary is still visible.
            double fragmentSupport[2]={};
            if(outside && visible<points*.45 && (onSensor(a)!=onSensor(b))){
                // Near-tangent clipping may leave only a bowed sliver of one
                // short edge. Densely sample the visible end along parallel
                // offsets and require a coherent paper/desk step on one
                // line, rather than accepting the image border as an edge.
                const auto anchor=onSensor(a)?a:b, tail=onSensor(a)?b:a;
                const int band=std::clamp(cvRound(sides[i]*.025),4,11);
                for(int t=-band;t<=band;++t){
                    int count=0, confirmed[2]={};
                    for(int n=0;n<28;++n){
                        const auto p=anchor+(tail-anchor)*float(.015+.31*(n+.5)/28)+normal*float(t);
                        const auto in=p+normal*2.5f,out=p-normal*2.5f;
                        if(!onSensor(in)||!onSensor(out))continue;
                        ++count;
                        const int dl=sample(gray,in)-sample(gray,out),dt=sample(tone,in)-sample(tone,out);
                        if(std::max(dl,dt)>=10)++confirmed[0];
                        if(std::max(-dl,-dt)>=10)++confirmed[1];
                    }
                    if(count>=7)for(int k=0;k<2;++k)
                        fragmentSupport[k]=std::max(fragmentSupport[k],.90*confirmed[k]/count);
                }
            }
            if (visible<(outside?3:points/3) && std::max(fragmentSupport[0],fragmentSupport[1])<.60) return;
            for(int k=0;k<2;++k){
                const double support=std::max(fragmentSupport[k],double(votes[k])/std::max(1,visible));
                bothPolarities[k]+=support;
                if(support>=0.46) ++strongSides[k];
            }
        }
        int polarity=bothPolarities[1]>bothPolarities[0]?1:0;
        const double support=bothPolarities[polarity]*0.25;
        if(strongSides[polarity]!=4 || support<0.57 || nearPaper<nearSamples*0.47) return;
        int paperPoints=0, sampleCount=0;
        for(int y=1;y<=7;++y) for(int x=1;x<=7;++x) {
            const float u=x/8.f, v=y/8.f;
            auto p=(q[0]*(1-u)+q[1]*u)*(1-v)+(q[3]*(1-u)+q[2]*u)*v;
            if(!onSensor(p))continue;
            const int lum=sample(gray,p), neutral=sample(tone,p);
            if(lum>=83 && (lum-neutral)<38 && neutral>=64) ++paperPoints;
            ++sampleCount;
        }
        const double interior=double(paperPoints)/sampleCount;
        if (interior < (polarity ? 0.62 : 0.42) || (outside && support<.69)) return;
        const double score=0.40*support+0.23*interior+0.14*ratioFit
                          +0.09*(1-worstCos)+0.14*std::sqrt(coverage)+(cut ? .08 : 0);
        if(score>best.score) best={q,score,cut!=nullptr,cut?*cut:Triangle{}};
    }

    void candidatesFromMask(const cv::Mat &mask)
    {
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(mask,contours,cv::RETR_LIST,cv::CHAIN_APPROX_SIMPLE);
        const auto minArea=gray.total()*0.075;
        for(const auto &contour : contours) {
            if(std::abs(cv::contourArea(contour))<minArea) continue;
            std::vector<cv::Point> hull, poly;
            // The true (possibly concave) contour preserves paper edge runs
            // separated by an object crossing the page. Its convex hull
            // alone would substitute an outer edge of that object.
            cv::approxPolyDP(contour,poly,cv::arcLength(contour,true)*.003,true);
            for(size_t i=0;i<poly.size();++i){
                const cv::Point2f a=poly[i], b=poly[(i+1)%poly.size()];
                const auto edge=b-a;
                const double span=length(edge);
                if(span>=14)contourEdges.push_back({a,edge/float(span),span});
            }
            cv::convexHull(contour,hull);
            const auto perimeter=cv::arcLength(hull,true);
            for(double epsilon:{0.006,0.012,0.022,0.038}) {
                cv::approxPolyDP(hull,poly,perimeter*epsilon,true);
                if(poly.size()==4) {
                    if(epsilon<.01)continue;
                    Quad q{cv::Point2f(poly[0]),cv::Point2f(poly[1]),cv::Point2f(poly[2]),cv::Point2f(poly[3])};
                    consider(q);
                    // A very short visible part of an edge can be merged
                    // into the frame boundary by polygon simplification.
                    for(int i=0;i<4;++i){
                        const auto p=q[i];
                        if(p.x>2 && p.y>2 && p.x<gray.cols-3 && p.y<gray.rows-3)continue;
                        auto inferred=q;
                        inferred[i]=q[(i+1)%4]+q[(i+3)%4]-q[(i+2)%4];
                        if(!onSensor(inferred[i]))consider(inferred);
                    }
                    break;
                }
                if(poly.size()==5)cornersFromBevel(poly);
            }
        }
    }

    static bool intersection(const Line &a,const Line &b,cv::Point2f &point)
    {
        const auto div=determinant(a.direction,b.direction);
        if(std::abs(div)<0.5) return false;
        point=a.start+a.direction*float(determinant(b.start-a.start,b.direction)/div);
        return true;
    }

    bool isCutEdge(cv::Point2f corner, cv::Point2f start, cv::Point2f end,
                   cv::Point2f adjacentA, cv::Point2f adjacentB) const
    {
        if(!onSensor(corner))return false;
        const double fractionA=length(corner-start)/length(corner-adjacentA);
        const double fractionB=length(corner-end)/length(corner-adjacentB);
        if(std::min(fractionA,fractionB)<.035 || std::max(fractionA,fractionB)>.52)return false;
        const auto mid=(start+end)*.5f, direction=end-start;
        const auto extent=length(direction);
        if(extent<12)return false;
        cv::Point2f inward(-direction.y/extent,direction.x/extent);
        if((corner-mid).dot(inward)>0)inward=-inward;
        if((mid-corner).dot(inward)<5)return false;
        // A diagonal of a convex hull could bridge a shadow or two unrelated
        // features. Check that real pixels across nearly the entire proposed
        // cut transition from paper to the same background toward the apex.
        auto rgbStep=[&](cv::Point2f a,cv::Point2f b){
            const auto x=color.at<cv::Vec3b>(cvRound(a.y),cvRound(a.x));
            const auto y=color.at<cv::Vec3b>(cvRound(b.y),cvRound(b.x));
            return std::max({std::abs(x[0]-y[0]),std::abs(x[1]-y[1]),std::abs(x[2]-y[2])});
        };
        // A printed dark corner also makes a pentagon in a paper-colour
        // mask. The desk must continue *across* both invisible page edges.
        // Compare nearby pixels in colour on each side of the virtual edge.
        for(auto cut:{start,end}){
            const auto edge=cut-corner;
            const auto span=length(edge);
            cv::Point2f normal(-edge.y/span,edge.x/span);
            int continuous=0,observed=0;
            for(int i=0;i<15;++i){
                const auto p=corner+edge*float(.18+.60*(i+.5)/15);
                const auto a=p+normal*3.f, b=p-normal*3.f;
                if(!onSensor(a)||!onSensor(b))continue;
                ++observed;
                if(rgbStep(a,b)<23)++continuous;
            }
            if(observed<6 || continuous<observed*.70)return false;
        }
        int lighter=0, darker=0;
        for(int i=0;i<22;++i){
            const auto p=start+direction*float(.1+.8*(i+.5)/22);
            const auto paper=p+inward*4.5f, outside=p-inward*4.5f, towardApex=p*.65f+corner*.35f;
            if(!onSensor(paper)||!onSensor(outside))continue;
            const int luminance=sample(gray,paper), color=sample(tone,paper);
            if(luminance<72 || luminance-color>47)continue;
            for(int sign:{1,-1}){
                auto evidence=[&](const cv::Mat &channel){
                    const int inside=sample(channel,paper);
                    return std::min(sign*(inside-sample(channel,outside)),sign*(inside-sample(channel,towardApex)));
                };
                if(std::max(evidence(gray),evidence(tone))>=8){
                    if(sign==1)++lighter;else ++darker;
                }
            }
        }
        return std::max(lighter,darker)>=17;
    }

    // Identify the diagonal independently from the four page sides. It may
    // belong to a non-convex silhouette when a strap obscures another edge.
    void findCutOnOutline()
    {
        const auto &q=best.corners;
        for(int i=0;i<4;++i){
            const auto corner=q[i],a=q[(i+3)%4],b=q[(i+1)%4];
            if(!onSensor(corner))continue;
            const auto da=a-corner,db=b-corner;
            const Line sideA{corner,da/float(length(da)),length(da)};
            const Line sideB{corner,db/float(length(db)),length(db)};
            for(const auto &piece:contourEdges){
                cv::Point2f u,v;
                // Intersections must fall on the two forward page rays.
                // The physical contour segment has to span most of the
                // proposed cut; short strokes inside the sheet cannot vote.
                if(!intersection(sideA,piece,u) || !intersection(sideB,piece,v))continue;
                if((u-corner).dot(da)<=0 || (v-corner).dot(db)<=0)continue;
                const double tu=(u-piece.start).dot(piece.direction),tv=(v-piece.start).dot(piece.direction);
                const double low=std::min(tu,tv),high=std::max(tu,tv);
                const double overlap=std::min(high,piece.extent)-std::max(low,0.0);
                if(overlap<.60*(high-low) || !isCutEdge(corner,u,v,a,b))continue;
                best.hasCut=true;
                best.cut={corner,u,v};
                return;
            }
        }
    }

    // A single clipped/torn corner replaces one vertex of the sheet with a
    // short fifth edge. Intersect its two neighbouring paper edges. A frame
    // boundary is evidence of clipping only, never a substitute paper edge.
    void cornersFromBevel(const std::vector<cv::Point> &polygon)
    {
        for(int i=0;i<5;++i){
            const cv::Point2f a=polygon[i], cut1=polygon[(i+1)%5], cut2=polygon[(i+2)%5],
                              b=polygon[(i+3)%5], opposite=polygon[(i+4)%5];
            const auto da=cut1-a, db=cut2-b;
            const double sa=length(da), sb=length(db);
            if(std::min(sa,sb)<10)continue;
            cv::Point2f corner;
            if(intersection({a,da/float(sa),sa},{b,db/float(sb),sb},corner)){
                if((corner-cut1).dot(da)>=-sa*2 && (corner-cut2).dot(db)>=-sb*2){
                    const Triangle cut{corner,cut1,cut2};
                    if(!onSensor(corner))consider({a,corner,b,opposite});
                    else if(isCutEdge(corner,cut1,cut2,a,b))consider({a,corner,b,opposite},&cut);
                }
            }
            // Under an overhead camera the opposite sides are close to
            // parallel. This second proposal handles a bowed or very short
            // fragment; its four visible edge runs still have to pass the
            // same independent paper/desk contrast checks.
            auto estimate=a+b-opposite;
            if(!onSensor(estimate))consider({a,estimate,b,opposite});
        }
    }

    // A convex hull follows an object protruding over a corner (often a
    // fingertip). Estimate each actual long paper edge from its unobstructed
    // central run, then intersect the adjacent fitted lines.
    void refineBoundary()
    {
        const Candidate coarse=best;
        std::array<Line,4> sides;
        for(int i=0;i<4;++i){
            auto a=coarse.corners[i], b=coarse.corners[(i+1)%4];
            const auto span=length(b-a);
            const cv::Point2f along=(b-a)/float(span), across(-along.y,along.x);
            sides[i]={a,along,span};
            const int band=std::clamp(int(span*.067),6,32);
            std::vector<cv::Point2f> samples;
            for(int n=0;n<36;++n){
                const auto seed=a+(b-a)*float(.08+.84*(n+.5)/36);
                float peak=4.f;
                cv::Point2f chosen;
                for(int t=-band;t<=band;++t){
                    const auto point=seed+across*float(t);
                    if(point.x<1 || point.y<1 || point.x>=gray.cols-1 || point.y>=gray.rows-1)continue;
                    float contrast=0;
                    for(const auto *channel:{&gray,&tone}){
                        const int d1=sample(*channel,point+across*2.f)-sample(*channel,point-across*2.f);
                        const int d2=sample(*channel,point+across*5.f)-sample(*channel,point-across*5.f);
                        if(d1*d2>0)contrast=std::max(contrast,float(std::min(std::abs(d1),std::abs(d2))));
                    }
                    // Favour a nearby boundary when strengths are comparable.
                    const float evidence=contrast/(1.f+.018f*std::abs(t));
                    if(evidence>peak){peak=evidence;chosen=point;}
                }
                if(peak>4.f)samples.push_back(chosen);
            }
            std::vector<cv::Point2f> most;
            for(size_t m=0;m<samples.size();m+=3)for(size_t n=m+1;n<samples.size();n+=3){
                auto dir=samples[n]-samples[m];
                const auto baseline=length(dir);
                if(baseline<span*.28)continue;
                dir/=float(baseline);
                std::vector<cv::Point2f> inliers;
                for(auto point:samples)
                    if(std::abs(determinant(point-samples[m],dir))<2.5)inliers.push_back(point);
                if(inliers.size()>most.size())most=std::move(inliers);
            }
            if(most.size()>=18){
                cv::Vec4f fit;
                cv::fitLine(most,fit,cv::DIST_L2,0,0.01,0.01);
                sides[i]={{fit[2],fit[3]},{fit[0],fit[1]},span};
            }
        }
        Quad refined;
        for(int i=0;i<4;++i){
            if(!intersection(sides[(i+3)%4],sides[i],refined[i]))return;
            if(length(refined[i]-coarse.corners[i])>std::min(gray.cols,gray.rows)*.10)return;
        }
        for(int i=0;i<4;++i){
            const auto corner=coarse.corners[i];
            const auto shift=length(refined[i]-corner);
            if(!onSensor(corner) || shift<2 || shift>std::min(gray.cols,gray.rows)*.035) continue;
            // On curled paper the long central segment does not always
            // intersect at the true endpoint. Keep the observed vertex when
            // both short edge runs meeting there still have clear support.
            int supportedRuns=0;
            for(int direction:{-1,1}){
                const auto edge=coarse.corners[(i+4+direction)%4]-corner;
                const auto n=cv::Point2f(-edge.y,edge.x)/float(length(edge));
                int support=0;
                for(int k=1;k<=6;++k){
                    const auto p=corner+edge*float(.014*k);
                    const auto a=p+n*2.8f,b=p-n*2.8f;
                    if(std::max(std::abs(sample(gray,a)-sample(gray,b)),
                                std::abs(sample(tone,a)-sample(tone,b)))>=10) ++support;
                }
                if(support>=4)++supportedRuns;
            }
            if(supportedRuns==2)refined[i]=corner;
        }
        best.score=0;
        Triangle cut=coarse.cut;
        if(coarse.hasCut){
            auto it=std::min_element(coarse.corners.begin(),coarse.corners.end(),[&](auto a,auto b){
                return length(a-cut[0])<length(b-cut[0]);
            });
            cut[0]=refined[it-coarse.corners.begin()];
        }
        consider(refined,coarse.hasCut?&cut:nullptr);
        if(best.score<coarse.score-.025)best=coarse;
    }

    void candidatesFromLines()
    {
        std::vector<cv::Vec4i> segments;
        const int shortest=std::min(gray.rows,gray.cols);
        cv::HoughLinesP(edges,segments,1,CV_PI/180,std::max(22,shortest/13),
                        shortest*0.21,shortest*0.035);
        std::vector<Line> lines;
        for(const auto &line:contourEdges)
            if(line.extent>=shortest*.13)lines.push_back(line);
        for(auto s:segments) {
            cv::Point2f a(s[0],s[1]), d(s[2]-s[0],s[3]-s[1]);
            const double extent=length(d);
            if (extent>0) lines.push_back({a,d/float(extent),extent});
        }
        // Prefer segments that separate two surfaces. Long printed table
        // borders otherwise crowd both page-edge directions out of the list.
        std::vector<std::pair<double,Line>> ranked;
        for(const auto &line:lines){
            const cv::Point2f n(-line.direction.y,line.direction.x);
            int light=0,dark=0;
            for(int j=0;j<16;++j){
                auto p=line.start+line.direction*float(line.extent*(j+.5)/16);
                if(!onSensor(p+n*6.f)||!onSensor(p-n*6.f))continue;
                for(int sign:{1,-1}){
                    auto step=[&](const cv::Mat &m,float d){
                        return sign*(sample(m,p+n*d)-sample(m,p-n*d));
                    };
                    if(std::max(std::min(step(gray,3),step(gray,6)),
                                std::min(step(tone,3),step(tone,6)))>=6){
                        if(sign==1)++light;else ++dark;
                    }
                }
            }
            const double support=std::max(light,dark)/16.;
            if(support>=.375)ranked.emplace_back(line.extent*(.5+support*.5),line);
        }
        std::sort(ranked.begin(),ranked.end(),[](const auto &a,const auto &b){return a.first>b.first;});
        std::vector<Line> kept;
        for(const auto &entry:ranked) {
            const auto &line=entry.second;
            const auto center=line.start+line.direction*float(line.extent*.5);
            bool duplicate=false;
            for(auto &k:kept) {
                if(std::abs(line.direction.dot(k.direction))>0.994
                   && std::abs(determinant(center-k.start,k.direction))<shortest*.022) {duplicate=true;break;}
            }
            if(!duplicate) kept.push_back(line);
            if(kept.size()>=20) break;
        }
        std::vector<std::pair<int,int>> pairs;
        for(int i=0;i<int(kept.size());++i) for(int j=i+1;j<int(kept.size());++j) {
            if(std::abs(kept[i].direction.dot(kept[j].direction))<0.90) continue;
            if(std::abs(determinant(kept[j].start-kept[i].start,kept[i].direction))<shortest*.18) continue;
            pairs.emplace_back(i,j);
        }
        for(size_t i=0;i<pairs.size();++i) for(size_t j=i+1;j<pairs.size();++j) {
            const auto &a=kept[pairs[i].first], &b=kept[pairs[i].second];
            const auto &c=kept[pairs[j].first], &d=kept[pairs[j].second];
            if(std::abs(a.direction.dot(c.direction))>0.62) continue;
            Quad q;
            if(intersection(a,c,q[0]) && intersection(a,d,q[1])
               && intersection(b,d,q[2]) && intersection(b,c,q[3])) consider(q);
        }
    }
};

QRect fitRect(const QSize &imageSize, const QRect &box)
{
    const QSize fit = imageSize.scaled(box.size(), Qt::KeepAspectRatio);
    return QRect(box.x() + (box.width() - fit.width()) / 2,
                 box.y() + (box.height() - fit.height()) / 2,
                 fit.width(), fit.height());
}

} // namespace

bool ImageProcessor::validCorners(const QVector<QPointF> &corners, bool allowPartial)
{
    if (corners.size() != 4)
        return false;
    double twiceArea = 0.0;
    int outside=0;
    for (int i = 0; i < 4; ++i) {
        const QPointF &a = corners[i];
        const QPointF &b = corners[(i + 1) % 4];
        const QPointF &c = corners[(i + 2) % 4];
        if (!std::isfinite(a.x()) || !std::isfinite(a.y()))
            return false;
        if(a.x()<0 || a.y()<0 || a.x()>1 || a.y()>1){
            if(!allowPartial || ++outside>1 || a.x() < -cornerOvershoot || a.y() < -cornerOvershoot ||
               a.x()>1+cornerOvershoot || a.y()>1+cornerOvershoot)return false;
        }
        // Positive winding in image coordinates is TL, TR, BR, BL. Requiring
        // every turn to agree also rejects crossing, concave and collinear quads.
        if (cross(a, b, c) <= 1e-7)
            return false;
        twiceArea += a.x() * b.y() - b.x() * a.y();
    }
    return twiceArea >= 0.0002;
}

bool ImageProcessor::validMissingTriangle(const QVector<QPointF> &corners,const QVector<QPointF> &triangle)
{
    if(triangle.isEmpty())return true;
    if(triangle.size()!=3 || !validCorners(corners,true))return false;
    for(auto p:triangle)
        if(!std::isfinite(p.x()) || !std::isfinite(p.y()) ||
           p.x()<0 || p.x()>1 || p.y()<0 || p.y()>1)return false;
    for(int i=0;i<4;++i){
        const auto corner=corners[i];
        if(std::hypot(corner.x()-triangle[0].x(),corner.y()-triangle[0].y())>1.e-5)continue;
        auto onSide=[&](QPointF point,QPointF end){
            const auto edge=end-corner, v=point-corner;
            const auto len2=QPointF::dotProduct(edge,edge);
            const auto t=QPointF::dotProduct(v,edge)/len2;
            const auto residual=v-edge*t;
            return t>.012 && t<.57 && std::hypot(residual.x(),residual.y())<.013;
        };
        const auto left=corners[(i+3)%4],right=corners[(i+1)%4];
        return (onSide(triangle[1],left)&&onSide(triangle[2],right)) ||
               (onSide(triangle[2],left)&&onSide(triangle[1],right));
    }
    return false;
}

QVector<QPointF> ImageProcessor::detectCorners(const QImage &image, QVector<QPointF> *missingTriangle)
{
    if(missingTriangle)missingTriangle->clear();
    if (image.width()<48 || image.height()<48) return {};
    constexpr int detectorSize=800;
    const QImage reduced = std::max(image.width(), image.height()) > detectorSize
        ? image.scaled(detectorSize, detectorSize, Qt::KeepAspectRatio, Qt::FastTransformation) : image;
    return SheetLocator(rgbCopy(reduced)).locate(missingTriangle);
}

QImage ImageProcessor::process(const QImage &image, const EditOptions &options)
{
    try {
        if(image.isNull()) throw std::runtime_error("图像为空，无法处理。");
        const QImage rgb=image.convertToFormat(QImage::Format_RGB888);
        if(rgb.isNull()) throw std::runtime_error("无法分配图像内存。");
        // Read the original scan without cloning the full 20 MP camera frame.
        // All transforms below allocate a new destination before writing.
        cv::Mat result(rgb.height(),rgb.width(),CV_8UC3,const_cast<uchar*>(rgb.constBits()),rgb.bytesPerLine());
        if (options.rotation % 90 != 0)
            throw std::runtime_error("旋转角度必须是 90 度的整数倍。");
        QVector<QPointF> corners = options.corners;
        QVector<QPointF> cut=options.missingTriangle;
        if (!corners.isEmpty() && !validCorners(corners,true))
            throw std::runtime_error("裁切四角无效，请按左上、右上、右下、左下排列，避免交叉。");
        if (corners.isEmpty() && options.autoCrop)
            corners = detectCorners(image,&cut);
        if(!validMissingTriangle(corners,cut))
            throw std::runtime_error("缺角补白区域无效，请重新识别纸边。");
        if (!corners.isEmpty()) {
            std::array<cv::Point2f, 4> source;
            for (int i = 0; i < 4; ++i)
                source[i] = cv::Point2f(static_cast<float>(corners[i].x() * (image.width() - 1)),
                                        static_cast<float>(corners[i].y() * (image.height() - 1)));
            double width = std::max(cv::norm(source[1] - source[0]),
                                          cv::norm(source[2] - source[3])) + 1.0;
            double height = std::max(cv::norm(source[3] - source[0]),
                                           cv::norm(source[2] - source[1])) + 1.0;
            const bool partial=std::any_of(corners.begin(),corners.end(),[](const QPointF &p){
                return p.x()<0 || p.x()>1 || p.y()<0 || p.y()>1;
            });
            if(partial || !cut.isEmpty()){
                const double a4Ratio=width>height ? 297.0/210.0 : 210.0/297.0;
                // Choose the native sampling density of the shorter axis.
                width=std::min(width,height*a4Ratio);
                height=width/a4Ratio;
            }
            // Preserve native sampling. Perspective correction should not
            // silently manufacture a higher pixel count than the source.
            const double scale = std::min(1.0, std::sqrt(
                (static_cast<double>(image.width()) * image.height()) / (width * height)));
            const int outputWidth = static_cast<int>(std::lround(width * scale));
            const int outputHeight = static_cast<int>(std::lround(height * scale));
            if (outputWidth < 2 || outputHeight < 2)
                throw std::runtime_error("裁切区域太小，请扩大四角范围。");
            const std::array<cv::Point2f, 4> destination = {
                cv::Point2f(0, 0), cv::Point2f(outputWidth - 1.0f, 0),
                cv::Point2f(outputWidth - 1.0f, outputHeight - 1.0f),
                cv::Point2f(0, outputHeight - 1.0f)};
            cv::Mat corrected;
            const cv::Mat transform=cv::getPerspectiveTransform(source.data(),destination.data());
            cv::warpPerspective(result, corrected,
                                transform,
                                cv::Size(outputWidth, outputHeight), cv::INTER_LINEAR,
                                cv::BORDER_CONSTANT,cv::Scalar::all(255));
            if(!cut.isEmpty()){
                std::vector<cv::Point2f> input, projected;
                for(auto p:cut)input.emplace_back(p.x()*(image.width()-1),p.y()*(image.height()-1));
                cv::perspectiveTransform(input,projected,transform);
                // Intersect the measured diagonal with the two *rectified*
                // page edges. This removes the whole desk wedge even when
                // refining the outer edges shifts their intersection a pixel.
                const auto apex=projected[0];
                const auto line=projected[2]-projected[1];
                if(std::abs(line.x)>.001 && std::abs(line.y)>.001){
                    const float x=apex.x<outputWidth*.5f?0:outputWidth-1;
                    const float y=apex.y<outputHeight*.5f?0:outputHeight-1;
                    auto atX=projected[1]+line*((x-projected[1].x)/line.x);
                    auto atY=projected[1]+line*((y-projected[1].y)/line.y);
                    std::array<cv::Point,3> mask{{{cvRound(x),cvRound(y)},{cvRound(x),cvRound(atX.y)},
                                                 {cvRound(atY.x),cvRound(y)}}};
                    cv::fillConvexPoly(corrected,mask.data(),3,cv::Scalar::all(255),cv::LINE_AA);
                }
            }
            result = corrected;
        }

        const int rotation = ((options.rotation % 360) + 360) % 360;
        if(rotation) {
            cv::Mat rotated;
            const int transform=rotation==90?cv::ROTATE_90_CLOCKWISE:
                                rotation==180?cv::ROTATE_180:cv::ROTATE_90_COUNTERCLOCKWISE;
            cv::rotate(result,rotated,transform);
            result=rotated;
        }

        switch (options.color) {
        case ColorMode::Color:
            break;
        case ColorMode::Gray:
            cv::cvtColor(result, result, cv::COLOR_RGB2GRAY);
            break;
        case ColorMode::BlackWhite: {
            cv::cvtColor(result, result, cv::COLOR_RGB2GRAY);
            const int shortestSide = std::min(result.rows, result.cols);
            const int blockSize = std::min(41, shortestSide % 2 ? shortestSide : shortestSide - 1);
            if (blockSize >= 3) {
                cv::GaussianBlur(result, result, cv::Size(3, 3), 0);
                cv::adaptiveThreshold(result, result, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                                      cv::THRESH_BINARY, blockSize, 15);
            } else {
                cv::threshold(result, result, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
            }
            break;
        }
        default:
            throw std::runtime_error("未知的图像色彩模式。");
        }
        return imageCopy(result);
    } catch (const cv::Exception &error) {
        throw std::runtime_error(std::string("图像处理失败：") + error.what());
    }
}

QImage ImageProcessor::composeIdCard(const QImage &front, const QImage &back)
{
    if (front.isNull() || back.isNull())
        throw std::runtime_error("请先准备身份证正面和反面图像。");
    QImage sheet(2480, 3508, QImage::Format_RGB888);
    if (sheet.isNull())
        throw std::runtime_error("无法创建身份证合并页面，可能没有足够内存。");
    sheet.fill(Qt::white);
    QPainter painter(&sheet);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    // Large white margins and two disjoint fit boxes preserve both aspect
    // ratios. This is an A4 composition, not a claim of physical card scale.
    const QRect upper(300, 420, 1880, 1180);
    const QRect lower(300, 1908, 1880, 1180);
    painter.drawImage(fitRect(front.size(), upper), front);
    painter.drawImage(fitRect(back.size(), lower), back);
    painter.end();
    return sheet;
}

} // namespace scan
